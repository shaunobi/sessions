#include <windows.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <shcore.h>
#undef GetCurrentTime
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.ApplicationModel.DataTransfer.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.UI.Xaml.Interop.h>
#include <winrt/Microsoft.UI.h>
#include <winrt/Microsoft.UI.Dispatching.h>
#include <winrt/Microsoft.UI.Text.h>
#include <winrt/Microsoft.UI.Windowing.h>
#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Microsoft.UI.Xaml.Automation.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.Controls.Primitives.h>
#include <winrt/Microsoft.UI.Xaml.Documents.h>
#include <winrt/Microsoft.UI.Xaml.Input.h>
#include <winrt/Microsoft.UI.Xaml.Markup.h>
#include <winrt/Microsoft.UI.Xaml.Media.h>
#include <winrt/Microsoft.UI.Xaml.Media.Imaging.h>
#include <winrt/Microsoft.UI.Xaml.XamlTypeInfo.h>
#include <algorithm>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include "AppIcon.h"
#include "AppTheme.h"
#include "Embedded.h"
#include "ResourceIds.h"
#include "Index.h"
#include "Indexer.h"
#include "Embedder.h"
#include "Launch.h"
#include "Search.h"
#include "Text.h"

using namespace winrt;
using namespace winrt::Windows::Foundation;
using namespace Microsoft::UI::Xaml;
using namespace Microsoft::UI::Xaml::Controls;
using namespace Microsoft::UI::Xaml::Media;
using namespace Microsoft::UI::Windowing;
namespace Collections = winrt::Windows::Foundation::Collections;
using sessions::Tool;

namespace {

FrameworkElement Xaml(std::wstring const& text) { return Markup::XamlReader::Load(text).as<FrameworkElement>(); }
template <typename T> T Named(FrameworkElement const& root, wchar_t const* name) { return root.FindName(name).as<T>(); }

Brush ThemeBrush(wchar_t const* key) {
    return Application::Current().Resources().Lookup(box_value(key)).as<Brush>();
}

std::wstring Number(int64_t n) {
    auto s = std::to_wstring(n);
    for (int i = int(s.size()) - 3; i > 0; i -= 3) s.insert(size_t(i), L",");
    return s;
}

std::wstring Lower(std::wstring text) {
    if (!text.empty()) CharLowerBuffW(text.data(), DWORD(text.size()));
    return text;
}

SYSTEMTIME LocalTime(int64_t ms) {
    ULARGE_INTEGER value{};
    value.QuadPart = uint64_t(ms + 11644473600000LL) * 10000ULL;
    FILETIME utc{value.LowPart, value.HighPart}, local{};
    SYSTEMTIME out{};
    FileTimeToLocalFileTime(&utc, &local);
    FileTimeToSystemTime(&local, &out);
    return out;
}

std::wstring Format(SYSTEMTIME const& time, wchar_t const* dateFormat, bool withTime) {
    wchar_t date[80]{}, clock[40]{};
    if (dateFormat) GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, 0, &time, dateFormat, date, 80, nullptr);
    if (withTime) GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, TIME_NOSECONDS, &time, nullptr, clock, 40);
    std::wstring out = date;
    if (withTime) out += (out.empty() ? L"" : L" ") + std::wstring(clock);
    return out;
}

// Today: the time. Yesterday, this week, this year, then the full date.
std::wstring FriendlyDate(int64_t ms) {
    if (ms <= 0) return L"";
    auto then = LocalTime(ms), now = LocalTime(sessions::NowMs());
    auto dayNumber = [](SYSTEMTIME t) {
        FILETIME f{};
        t.wHour = t.wMinute = t.wSecond = t.wMilliseconds = 0;
        SystemTimeToFileTime(&t, &f);
        return int64_t((ULARGE_INTEGER{f.dwLowDateTime, f.dwHighDateTime}.QuadPart) / 864000000000ULL);
    };
    auto days = dayNumber(now) - dayNumber(then);
    if (days == 0) return Format(then, nullptr, true);
    if (days == 1) return L"Yesterday " + Format(then, nullptr, true);
    if (days > 1 && days < 7) return Format(then, L"dddd", true);
    if (then.wYear == now.wYear) return Format(then, L"MMM d", false);
    return Format(then, L"MMM d, yyyy", false);
}

winrt::Windows::UI::Color Rgb(uint8_t r, uint8_t g, uint8_t b) { return {255, r, g, b}; }

struct ToolLook {
    winrt::Windows::UI::Color color;
    wchar_t const* glyph;
};

ToolLook LookOf(Tool tool) {
    // Claude coral and OpenAI green; a monitor for desktop apps, a prompt for the command line.
    switch (tool) {
    case Tool::ClaudeDesktop: return {Rgb(0xD9, 0x77, 0x57), L""};
    case Tool::ClaudeCli: return {Rgb(0xB8, 0x5C, 0x3E), L""};
    case Tool::CodexDesktop: return {Rgb(0x10, 0xA3, 0x7F), L""};
    case Tool::CodexCli: return {Rgb(0x0B, 0x7A, 0x5F), L""};
    }
    return {Rgb(0x60, 0x60, 0x60), L""};
}

using sessions::Found;

// A PNG embedded in the executable as an image source; nullptr when missing.
// `size` is the logical size it will be drawn at: Windows' decoder scales the
// large source down with high quality filtering (including display scaling),
// instead of XAML shrinking a 256-pixel bitmap at draw time, which leaves
// jagged edges.
Imaging::BitmapImage EmbeddedImage(int id, int size = 0) {
    auto bytes = sessions::EmbeddedFile(id);
    if (bytes.empty()) return nullptr;
    com_ptr<IStream> memory;
    memory.attach(SHCreateMemStream(reinterpret_cast<BYTE const*>(bytes.data()), UINT(bytes.size())));
    if (!memory) return nullptr;
    winrt::Windows::Storage::Streams::IRandomAccessStream stream{nullptr};
    if (FAILED(CreateRandomAccessStreamOverStream(memory.get(), BSOS_DEFAULT, guid_of<winrt::Windows::Storage::Streams::IRandomAccessStream>(),
                                                  put_abi(stream))))
        return nullptr;
    Imaging::BitmapImage image;
    if (size > 0) {
        image.DecodePixelType(Imaging::DecodePixelType::Logical);
        image.DecodePixelWidth(size);
        image.DecodePixelHeight(size);
    }
    image.SetSource(stream);
    return image;
}

// Runs searches on its own thread and database connection so typing never waits
// on the index. Only the latest query's results are delivered.
class Searcher {
public:
    using Done = std::function<void(uint64_t, std::wstring, std::vector<Found>)>;

    Searcher(std::filesystem::path database, Done done) : file(std::move(database)), deliver(std::move(done)) {
        worker = std::thread([this] { Run(); });
    }
    ~Searcher() {
        {
            std::lock_guard lock(mutex);
            stopping = true;
        }
        wake.notify_one();
        worker.join();
    }
    uint64_t Ask(std::wstring query) {
        std::lock_guard lock(mutex);
        pending = std::move(query);
        hasPending = true;
        wake.notify_one();
        return ++generation;
    }

private:
    std::filesystem::path file;
    Done deliver;
    std::thread worker;
    std::mutex mutex;
    std::condition_variable wake;
    std::wstring pending;
    bool hasPending = false, stopping = false;
    uint64_t generation = 0;

    void Run() {
        std::unique_ptr<sessions::HybridSearch> search;
        for (;;) {
            std::wstring query;
            uint64_t ticket{};
            {
                std::unique_lock lock(mutex);
                wake.wait(lock, [&] { return hasPending || stopping; });
                if (stopping) return;
                query = std::move(pending);
                hasPending = false;
                ticket = generation;
            }
            std::vector<Found> results;
            try {
                if (!search) search = std::make_unique<sessions::HybridSearch>(file);
                results = search->Run(query);
            } catch (...) {
            }
            deliver(ticket, query, std::move(results));
        }
    }
};

constexpr wchar_t ShellMarkup[] = LR"(
<Grid xmlns='http://schemas.microsoft.com/winfx/2006/xaml/presentation' xmlns:x='http://schemas.microsoft.com/winfx/2006/xaml' Background='Transparent'>
  <Grid.RowDefinitions><RowDefinition Height='48'/><RowDefinition Height='*'/></Grid.RowDefinitions>
  <Grid Grid.Row='0'>
    <Grid.ColumnDefinitions><ColumnDefinition Width='*'/><ColumnDefinition Width='Auto'/><ColumnDefinition Width='140'/></Grid.ColumnDefinitions>
    <Grid x:Name='TitleArea' Background='Transparent'>
      <StackPanel Orientation='Horizontal' Spacing='10' VerticalAlignment='Center' Margin='16,0,0,0'>
        <Image x:Name='BrandIcon' Width='20' Height='20' Stretch='Uniform'/>
        <TextBlock Text='Sessions' FontWeight='SemiBold' VerticalAlignment='Center'/>
      </StackPanel>
    </Grid>
    <StackPanel Grid.Column='1' Orientation='Horizontal' Spacing='8' VerticalAlignment='Center' Margin='0,0,8,0'>
      <ProgressRing x:Name='Busy' Width='14' Height='14' MinWidth='14' MinHeight='14' IsActive='False' Visibility='Collapsed'/>
      <FontIcon x:Name='Done' Glyph='&#xE73E;' FontSize='13' Foreground='{ThemeResource SystemFillColorSuccessBrush}' Visibility='Collapsed'/>
      <FontIcon x:Name='Problem' Glyph='&#xE7BA;' FontSize='13' Foreground='{ThemeResource SystemFillColorCautionBrush}' Visibility='Collapsed'/>
      <TextBlock x:Name='Status' FontSize='12' VerticalAlignment='Center' Foreground='{ThemeResource TextFillColorSecondaryBrush}' Text='Starting'/>
      <Button x:Name='Gear' Width='32' Height='32' Padding='0' Background='Transparent' BorderThickness='0' AutomationProperties.Name='Settings' ToolTipService.ToolTip='Settings'>
        <FontIcon Glyph='&#xE713;' FontSize='14'/>
      </Button>
    </StackPanel>
  </Grid>
  <SplitView x:Name='Split' Grid.Row='1' PanePlacement='Right' DisplayMode='Overlay' OpenPaneLength='380' IsPaneOpen='False'
             PaneBackground='{ThemeResource AcrylicInAppFillColorDefaultBrush}'>
    <SplitView.Pane>
      <ScrollViewer HorizontalScrollBarVisibility='Disabled'>
        <StackPanel Margin='20,8,20,24' Spacing='16'>
          <Grid>
            <TextBlock Text='Settings' FontSize='20' FontWeight='SemiBold' VerticalAlignment='Center'/>
            <Button x:Name='CloseSettings' HorizontalAlignment='Right' Background='Transparent' BorderThickness='0' AutomationProperties.Name='Close settings'>
              <FontIcon Glyph='&#xE711;' FontSize='12'/>
            </Button>
          </Grid>
          <TextBlock Text='Accounts' FontWeight='SemiBold'/>
          <TextBlock Text='Name each account as you know it. Rebuild re-reads that account&#8217;s sessions from disk.' TextWrapping='Wrap' FontSize='12' Foreground='{ThemeResource TextFillColorSecondaryBrush}'/>
          <StackPanel x:Name='AccountsPanel' Spacing='10'/>
          <TextBlock Text='Index' FontWeight='SemiBold' Margin='0,8,0,0'/>
          <TextBlock x:Name='IndexInfo' TextWrapping='Wrap' FontSize='12' Foreground='{ThemeResource TextFillColorSecondaryBrush}' IsTextSelectionEnabled='True'/>
          <StackPanel Orientation='Horizontal' Spacing='8'>
            <Button x:Name='RebuildAll' Content='Rebuild everything'/>
            <Button x:Name='OpenFolder' Content='Open folder'/>
          </StackPanel>
        </StackPanel>
      </ScrollViewer>
    </SplitView.Pane>
    <Grid>
      <Grid.RowDefinitions><RowDefinition Height='Auto'/><RowDefinition Height='Auto'/><RowDefinition Height='*'/></Grid.RowDefinitions>
      <TextBox x:Name='Search' Margin='24,4,24,10' PlaceholderText='Search what you or the AI said' AutomationProperties.Name='Search sessions'/>
      <Grid Grid.Row='1' Margin='24,0,24,10' ColumnSpacing='8'>
        <Grid.ColumnDefinitions>
          <ColumnDefinition Width='Auto'/><ColumnDefinition Width='Auto'/><ColumnDefinition Width='Auto'/><ColumnDefinition Width='Auto'/><ColumnDefinition Width='*'/>
        </Grid.ColumnDefinitions>
        <ComboBox x:Name='ToolFilter' MinWidth='140' AutomationProperties.Name='Tool'/>
        <ComboBox x:Name='AccountFilter' Grid.Column='1' MinWidth='150' MaxWidth='260' AutomationProperties.Name='Account'/>
        <ComboBox x:Name='TagFilter' Grid.Column='2' MinWidth='110' AutomationProperties.Name='Tag'/>
        <ComboBox x:Name='SortBox' Grid.Column='3' MinWidth='140' AutomationProperties.Name='Sort'/>
        <TextBlock x:Name='CountText' Grid.Column='4' HorizontalAlignment='Right' VerticalAlignment='Center' FontSize='12' Foreground='{ThemeResource TextFillColorSecondaryBrush}'/>
      </Grid>
      <GridView x:Name='Cards' Grid.Row='2' SelectionMode='None' IsItemClickEnabled='True' Padding='20,0,12,16'>
        <GridView.ItemContainerStyle>
          <Style TargetType='GridViewItem'>
            <Setter Property='HorizontalContentAlignment' Value='Stretch'/>
            <Setter Property='Padding' Value='0'/>
            <Setter Property='Margin' Value='0,0,10,10'/>
            <Setter Property='MinHeight' Value='0'/>
          </Style>
        </GridView.ItemContainerStyle>
      </GridView>
      <TextBlock x:Name='Empty' Grid.Row='2' HorizontalAlignment='Center' VerticalAlignment='Center' Visibility='Collapsed'
                 Foreground='{ThemeResource TextFillColorSecondaryBrush}' TextWrapping='Wrap' TextAlignment='Center' MaxWidth='420'/>
    </Grid>
  </SplitView>
</Grid>)";

}  // namespace

struct App : ApplicationT<App, Markup::IXamlMetadataProvider> {
    Microsoft::UI::Xaml::XamlTypeInfo::XamlControlsXamlMetaDataProvider provider{nullptr};
    Markup::IXamlType GetXamlType(winrt::Windows::UI::Xaml::Interop::TypeName const& type) { return provider.GetXamlType(type); }
    Markup::IXamlType GetXamlType(hstring const& name) { return provider.GetXamlType(name); }
    com_array<Markup::XmlnsDefinition> GetXmlnsDefinitions() { return provider.GetXmlnsDefinitions(); }

    Window window{nullptr};
    FrameworkElement shell{nullptr};
    sessions::AppIcon appIcon;
    TextBox search{nullptr};
    ComboBox toolFilter{nullptr}, accountFilter{nullptr}, tagFilter{nullptr}, sortBox{nullptr};
    GridView cards{nullptr};
    TextBlock countText{nullptr}, statusText{nullptr}, emptyText{nullptr}, indexInfo{nullptr};
    ProgressRing busy{nullptr};
    FontIcon doneIcon{nullptr}, problemIcon{nullptr};
    SplitView split{nullptr};
    StackPanel accountsPanel{nullptr};
    MenuFlyout cardMenu{nullptr};
    Imaging::BitmapImage claudeLogo{nullptr}, claudeCliLogo{nullptr}, chatgptLogo{nullptr};
    Collections::IObservableVector<IInspectable> items{nullptr};
    Microsoft::UI::Dispatching::DispatcherQueueTimer searchDelay{nullptr};

    std::filesystem::path databaseFile;
    std::unique_ptr<sessions::Index> index;
    std::unique_ptr<sessions::Indexer> indexer;
    std::unique_ptr<Searcher> searcher;

    std::vector<sessions::SessionRow> sessionRows;
    std::unordered_map<int64_t, size_t> rowById;
    std::vector<sessions::AccountRow> accountRows;
    std::unordered_map<std::string, std::wstring> accountNames;
    std::vector<std::wstring> tagList;
    std::vector<std::string> accountChoices;  // parallel to AccountFilter items after "All"

    std::wstring activeQuery;
    uint64_t searchTicket = 0;
    bool searching = false;
    std::unordered_map<int64_t, Found> found;

    struct CardCache {
        std::wstring signature;
        Border element{nullptr};
    };
    std::unordered_map<int64_t, CardCache> cardCache;
    std::vector<int64_t> shown;
    bool closing = false, populatingFilters = false, sortChosen = false;
    int lastTotal = 0;

    App() {
        provider = Microsoft::UI::Xaml::XamlTypeInfo::XamlControlsXamlMetaDataProvider();
        UnhandledException([](auto&&, UnhandledExceptionEventArgs const& e) {
            e.Handled(true);
            MessageBoxW(nullptr, e.Message().c_str(), L"Sessions", MB_OK | MB_ICONERROR);
        });
    }

    void OnLaunched(LaunchActivatedEventArgs const&) {
        try {
            Launch();
        } catch (hresult_error const& e) {
            MessageBoxW(nullptr, e.message().c_str(), L"Sessions could not start", MB_OK | MB_ICONERROR);
        } catch (std::exception const& e) {
            MessageBoxW(nullptr, sessions::Wide(e.what()).c_str(), L"Sessions could not start", MB_OK | MB_ICONERROR);
        }
    }

    void Launch() {
        Resources().MergedDictionaries().Append(XamlControlsResources());
        sessions::ApplyAppTheme(Application::Current());

        window = Window();
        window.Title(L"Sessions");
        appIcon.Set(window);
        window.SystemBackdrop(DesktopAcrylicBackdrop());
        shell = Xaml(ShellMarkup);
        window.Content(shell);
        window.ExtendsContentIntoTitleBar(true);
        window.AppWindow().TitleBar().PreferredHeightOption(TitleBarHeightOption::Tall);
        window.SetTitleBar(Named<Grid>(shell, L"TitleArea"));
        const double scale = GetDpiForSystem() / 96.0;
        window.AppWindow().Resize({int32_t(1060 * scale), int32_t(780 * scale)});

        Named<Image>(shell, L"BrandIcon").Source(EmbeddedImage(IDR_BRAND, 20));
        claudeLogo = EmbeddedImage(IDR_LOGO_CLAUDE, 32);
        claudeCliLogo = EmbeddedImage(IDR_LOGO_CLAUDE_CLI, 32);
        chatgptLogo = EmbeddedImage(IDR_LOGO_CHATGPT, 21);

        search = Named<TextBox>(shell, L"Search");
        toolFilter = Named<ComboBox>(shell, L"ToolFilter");
        accountFilter = Named<ComboBox>(shell, L"AccountFilter");
        tagFilter = Named<ComboBox>(shell, L"TagFilter");
        sortBox = Named<ComboBox>(shell, L"SortBox");
        cards = Named<GridView>(shell, L"Cards");
        countText = Named<TextBlock>(shell, L"CountText");
        statusText = Named<TextBlock>(shell, L"Status");
        emptyText = Named<TextBlock>(shell, L"Empty");
        indexInfo = Named<TextBlock>(shell, L"IndexInfo");
        busy = Named<ProgressRing>(shell, L"Busy");
        doneIcon = Named<FontIcon>(shell, L"Done");
        problemIcon = Named<FontIcon>(shell, L"Problem");
        split = Named<SplitView>(shell, L"Split");
        accountsPanel = Named<StackPanel>(shell, L"AccountsPanel");

        items = single_threaded_observable_vector<IInspectable>();
        cards.ItemsSource(items);
        cards.ItemClick([this](auto&&, ItemClickEventArgs const& e) {
            if (auto card = e.ClickedItem().try_as<FrameworkElement>()) Open(unbox_value<int64_t>(card.Tag()));
        });
        BuildCardMenu();

        toolFilter.Items().Append(box_value(L"All tools"));
        for (auto tool : {Tool::ClaudeDesktop, Tool::ClaudeCli, Tool::CodexDesktop, Tool::CodexCli})
            toolFilter.Items().Append(box_value(sessions::ToolName(tool)));
        toolFilter.SelectedIndex(0);
        for (auto label : {L"Last activity", L"Best match", L"Created", L"Most messages"}) sortBox.Items().Append(box_value(label));
        sortBox.SelectedIndex(0);
        for (auto combo : {toolFilter, accountFilter, tagFilter})
            combo.SelectionChanged([this](auto&&, auto&&) { if (!populatingFilters) Render(); });
        sortBox.SelectionChanged([this](auto&&, auto&&) {
            if (populatingFilters) return;
            sortChosen = true;
            Render();
        });

        searchDelay = window.DispatcherQueue().CreateTimer();
        searchDelay.Interval(std::chrono::milliseconds(180));
        searchDelay.IsRepeating(false);
        searchDelay.Tick([this](auto&&, auto&&) { StartSearch(); });
        search.TextChanged([this](auto&&, auto&&) {
            searchDelay.Stop();
            searchDelay.Start();
        });

        Named<Button>(shell, L"Gear").Click([this](auto&&, auto&&) { ToggleSettings(); });
        Named<Button>(shell, L"CloseSettings").Click([this](auto&&, auto&&) { split.IsPaneOpen(false); });
        Named<Button>(shell, L"RebuildAll").Click([this](auto&&, auto&&) {
            if (indexer) indexer->RebuildAll();
            split.IsPaneOpen(false);
        });
        Named<Button>(shell, L"OpenFolder").Click([this](auto&&, auto&&) { sessions::RevealFile(databaseFile.wstring()); });

        AddShortcut(winrt::Windows::System::VirtualKey::F, winrt::Windows::System::VirtualKeyModifiers::Control, [this] {
            search.Focus(FocusState::Keyboard);
            search.SelectAll();
        });
        AddShortcut(winrt::Windows::System::VirtualKey::Escape, winrt::Windows::System::VirtualKeyModifiers::None, [this] {
            if (split.IsPaneOpen()) split.IsPaneOpen(false);
            else if (!search.Text().empty()) search.Text(L"");
        });

        databaseFile = sessions::Index::DefaultFile();
        index = std::make_unique<sessions::Index>(databaseFile);
        Reload();

        auto queue = window.DispatcherQueue();
        auto weak = get_weak();
        searcher = std::make_unique<Searcher>(databaseFile, [queue, weak](uint64_t ticket, std::wstring query, std::vector<Found> results) {
            queue.TryEnqueue([weak, ticket, query = std::move(query), results = std::move(results)]() mutable {
                if (auto app = weak.get(); app && !app->closing) app->SearchDone(ticket, query, std::move(results));
            });
        });
        indexer = std::make_unique<sessions::Indexer>(databaseFile, [queue, weak](sessions::IndexStatus const& status) {
            queue.TryEnqueue([weak, status] {
                if (auto app = weak.get(); app && !app->closing) app->ShowStatus(status);
            });
        });

        window.Closed([this](auto&&, auto&&) {
            closing = true;
            searchDelay.Stop();
            if (indexer) indexer->Stop();
            indexer.reset();
            searcher.reset();
            appIcon.Reset();
        });
        window.Activate();
        search.Focus(FocusState::Programmatic);
        indexer->Start();
    }

    template <typename F>
    void AddShortcut(winrt::Windows::System::VirtualKey key, winrt::Windows::System::VirtualKeyModifiers modifiers, F action) {
        Input::KeyboardAccelerator accelerator;
        accelerator.Key(key);
        accelerator.Modifiers(modifiers);
        accelerator.Invoked([action](auto&&, Input::KeyboardAcceleratorInvokedEventArgs const& e) {
            action();
            e.Handled(true);
        });
        shell.KeyboardAccelerators().Append(accelerator);
        shell.KeyboardAcceleratorPlacementMode(Input::KeyboardAcceleratorPlacementMode::Hidden);
    }

    // ---- Data -------------------------------------------------------------

    void Reload() {
        try {
            sessionRows = index->LoadSessions();
            accountRows = index->Accounts();
            tagList = index->AllTags();
        } catch (std::exception const& e) {
            ShowProblem(L"Could not read the index: " + sessions::Wide(e.what()));
            return;
        }
        rowById.clear();
        for (size_t i = 0; i < sessionRows.size(); ++i) rowById[sessionRows[i].id] = i;
        accountNames.clear();
        for (auto const& account : accountRows) accountNames[account.id] = account.name;
        PopulateFilters();
        Render();
        if (split.IsPaneOpen()) BuildSettings();
    }

    void PopulateFilters() {
        populatingFilters = true;
        std::string selectedAccount;
        if (int i = accountFilter.SelectedIndex(); i > 0 && size_t(i - 1) < accountChoices.size()) selectedAccount = accountChoices[size_t(i - 1)];
        std::wstring selectedTag;
        if (int i = tagFilter.SelectedIndex(); i > 0) selectedTag = unbox_value<hstring>(tagFilter.SelectedItem()).c_str();

        accountFilter.Items().Clear();
        accountChoices.clear();
        accountFilter.Items().Append(box_value(L"All accounts"));
        int accountIndex = 0;
        for (auto const& account : accountRows) {
            accountChoices.push_back(account.id);
            accountFilter.Items().Append(box_value(account.name));
            if (account.id == selectedAccount) accountIndex = int(accountChoices.size());
        }
        accountFilter.SelectedIndex(accountIndex);

        tagFilter.Items().Clear();
        tagFilter.Items().Append(box_value(L"All tags"));
        int tagIndex = 0;
        for (size_t i = 0; i < tagList.size(); ++i) {
            tagFilter.Items().Append(box_value(tagList[i]));
            if (tagList[i] == selectedTag) tagIndex = int(i + 1);
        }
        tagFilter.SelectedIndex(tagIndex);
        tagFilter.IsEnabled(!tagList.empty());
        populatingFilters = false;
    }

    std::wstring AccountName(std::string const& id) {
        auto it = accountNames.find(id);
        return it != accountNames.end() ? it->second : sessions::DefaultAccountName(id);
    }

    // ---- Search -----------------------------------------------------------

    void StartSearch() {
        std::wstring query = search.Text().c_str();
        while (!query.empty() && iswspace(query.back())) query.pop_back();
        while (!query.empty() && iswspace(query.front())) query.erase(query.begin());
        if (query.empty()) {
            activeQuery.clear();
            searching = false;
            found.clear();
            SetSort(0);
            Render();
            return;
        }
        if (query == activeQuery && !searching) return;
        searching = true;
        searchTicket = searcher->Ask(query);
        countText.Text(L"Searching…");
    }

    void SearchDone(uint64_t ticket, std::wstring const& query, std::vector<Found> results) {
        if (ticket != searchTicket) return;
        searching = false;
        activeQuery = query;
        found.clear();
        for (auto& result : results) found[result.session] = std::move(result);
        SetSort(1);
        Render();
    }

    // Searching sorts by best match and clearing returns to last activity,
    // unless the person chose a sort themselves.
    void SetSort(int choice) {
        if (sortChosen || sortBox.SelectedIndex() == choice) return;
        populatingFilters = true;
        sortBox.SelectedIndex(choice);
        populatingFilters = false;
    }

    // ---- Cards ------------------------------------------------------------

    bool Matches(sessions::SessionRow const& row) {
        if (int tool = toolFilter.SelectedIndex(); tool > 0 && int(row.tool) != tool) return false;
        if (int account = accountFilter.SelectedIndex(); account > 0 && size_t(account - 1) < accountChoices.size() &&
            row.account != accountChoices[size_t(account - 1)])
            return false;
        if (int tag = tagFilter.SelectedIndex(); tag > 0 && size_t(tag - 1) < tagList.size() &&
            std::find(row.tags.begin(), row.tags.end(), tagList[size_t(tag - 1)]) == row.tags.end())
            return false;
        if (!activeQuery.empty() && !found.count(row.id)) return false;
        return true;
    }

    void Render() {
        std::vector<sessions::SessionRow const*> visible;
        for (auto const& row : sessionRows)
            if (Matches(row)) visible.push_back(&row);
        int sort = sortBox.SelectedIndex();
        auto byActivity = [](auto a, auto b) { return a->lastActivity > b->lastActivity; };
        if (sort == 1 && !activeQuery.empty())
            std::stable_sort(visible.begin(), visible.end(), [this](auto a, auto b) { return found[a->id].score > found[b->id].score; });
        else if (sort == 2)
            std::stable_sort(visible.begin(), visible.end(), [](auto a, auto b) { return a->created > b->created; });
        else if (sort == 3)
            std::stable_sort(visible.begin(), visible.end(), [](auto a, auto b) { return a->messages > b->messages; });
        else
            std::stable_sort(visible.begin(), visible.end(), byActivity);

        constexpr size_t limit = 600;
        std::vector<int64_t> ids;
        std::vector<IInspectable> elements;
        for (size_t i = 0; i < visible.size() && i < limit; ++i) {
            ids.push_back(visible[i]->id);
            elements.push_back(CardFor(*visible[i]));
        }
        if (ids != shown) {
            // A new search or filter starts at the top; a live update keeps its place.
            bool newList = shown.empty() || ids.empty() || ids.front() != shown.front();
            items.ReplaceAll(elements);
            shown = std::move(ids);
            if (newList && items.Size() > 0) cards.ScrollIntoView(items.GetAt(0));
        }

        std::wstring count;
        if (!activeQuery.empty())
            count = Number(int64_t(visible.size())) + (visible.size() == 1 ? L" session matches" : L" sessions match");
        else
            count = Number(int64_t(visible.size())) + (visible.size() == 1 ? L" session" : L" sessions");
        if (visible.size() > limit) count += L" · showing the first " + Number(limit);
        countText.Text(count);

        if (visible.empty()) {
            emptyText.Text(sessionRows.empty() ? L"Reading your Claude and Codex sessions…"
                           : !activeQuery.empty() ? L"No session mentions that. Try fewer or different words."
                                                  : L"No sessions match these filters.");
            emptyText.Visibility(Visibility::Visible);
        } else {
            emptyText.Visibility(Visibility::Collapsed);
        }
    }

    IInspectable CardFor(sessions::SessionRow const& row) {
        std::wstring snippet;
        if (!activeQuery.empty())
            if (auto it = found.find(row.id); it != found.end())
                snippet = (it->second.byMeaning ? L"\x03" : L"") + it->second.snippet;
        std::wstring signature = row.title + L"\x1F" + row.folder + L"\x1F" + AccountName(row.account) + L"\x1F" + std::to_wstring(row.messages) + L"\x1F" +
                                 std::to_wstring(row.lastActivity) + L"\x1F" + std::to_wstring(int(row.tool)) + L"\x1F" + snippet +
                                 (row.archived ? L"\x1F" L"a" : L"");
        for (auto const& tag : row.tags) signature += L"\x1F#" + tag;
        auto& cached = cardCache[row.id];
        if (!cached.element) {
            cached.element = Border();
            cached.element.Tag(box_value(row.id));
            cached.element.CornerRadius({8, 8, 8, 8});
            cached.element.BorderThickness({1, 1, 1, 1});
            cached.element.Padding({14, 12, 14, 12});
            cached.element.Width(300);
            cached.element.Background(ThemeBrush(L"CardBackgroundFillColorDefaultBrush"));
            cached.element.BorderBrush(ThemeBrush(L"CardStrokeColorDefaultBrush"));
            cached.element.ContextFlyout(cardMenu);
        }
        // Uniform tiles; taller while searching to make room for the passage.
        cached.element.Height(activeQuery.empty() ? (row.tags.empty() ? 104 : 130) : 168);
        if (cached.signature != signature) {
            cached.signature = signature;
            cached.element.Child(CardContent(row, snippet));
            Automation::AutomationProperties::SetName(cached.element, hstring(row.title + L", " + sessions::ToolName(row.tool)));
        }
        return cached.element;
    }

    // A fixed-size tile: badge and two-line title, then tool, account, date and
    // size, then the matching passage when searching, and tags at the bottom.
    UIElement CardContent(sessions::SessionRow const& row, std::wstring const& snippet) {
        Grid grid;
        grid.RowSpacing(6);
        for (auto height : {GridLengthHelper::Auto(), GridLengthHelper::Auto(), GridLengthHelper::FromValueAndType(1, GridUnitType::Star),
                            GridLengthHelper::Auto()}) {
            RowDefinition definition;
            definition.Height(height);
            grid.RowDefinitions().Append(definition);
        }

        Grid header;
        header.ColumnSpacing(10);
        ColumnDefinition badgeColumn, titleColumn;
        badgeColumn.Width(GridLengthHelper::FromPixels(32));
        titleColumn.Width(GridLengthHelper::FromValueAndType(1, GridUnitType::Star));
        header.ColumnDefinitions().Append(badgeColumn);
        header.ColumnDefinitions().Append(titleColumn);
        auto badge = Badge(row.tool);
        ToolTipService::SetToolTip(badge, box_value(std::wstring(sessions::ToolName(row.tool)) + L"  \u00B7  " + AccountName(row.account)));
        header.Children().Append(badge);
        TextBlock title;
        title.Text(row.title);
        title.FontWeight(Microsoft::UI::Text::FontWeights::SemiBold());
        title.TextWrapping(TextWrapping::Wrap);
        title.TextTrimming(TextTrimming::CharacterEllipsis);
        title.MaxLines(2);
        ToolTipService::SetToolTip(title, box_value(row.title));
        Grid::SetColumn(title, 1);
        header.Children().Append(title);
        grid.Children().Append(header);

        StackPanel meta;
        Grid::SetRow(meta, 1);
        auto line = [&](std::wstring const& value) {
            TextBlock block;
            block.Text(value);
            block.FontSize(12);
            block.Foreground(ThemeBrush(L"TextFillColorSecondaryBrush"));
            block.TextTrimming(TextTrimming::CharacterEllipsis);
            block.MaxLines(1);
            meta.Children().Append(block);
            return block;
        };
        std::wstring when = FriendlyDate(row.lastActivity) + L"  \u00B7  " + Number(row.messages) +
                            (row.messages == 1 ? L" message" : L" messages");
        if (row.archived) when += L"  \u00B7  archived";
        auto dateLine = line(L"");
        if (!row.folder.empty()) {
            // Folder glyph, folder name, then date and size.
            Documents::Run icon;
            icon.Text(L"\uE8B7 ");
            icon.FontFamily(FontFamily(L"Segoe Fluent Icons"));
            dateLine.Inlines().Append(icon);
            Documents::Run folder;
            folder.Text(row.folder + L"  \u00B7  ");
            folder.FontWeight(Microsoft::UI::Text::FontWeights::SemiBold());
            dateLine.Inlines().Append(folder);
            ToolTipService::SetToolTip(dateLine, box_value(row.cwd));
        }
        Documents::Run rest;
        rest.Text(when);
        dateLine.Inlines().Append(rest);
        grid.Children().Append(meta);

        if (!snippet.empty()) {
            auto passage = SnippetBlock(snippet);
            Grid::SetRow(passage, 2);
            grid.Children().Append(passage);
        }

        if (!row.tags.empty()) {
            StackPanel chips;
            chips.Orientation(Orientation::Horizontal);
            chips.Spacing(6);
            Grid::SetRow(chips, 3);
            for (auto const& tag : row.tags) {
                Border chip;
                chip.CornerRadius({9, 9, 9, 9});
                chip.Padding({8, 1, 8, 2});
                chip.Background(ThemeBrush(L"SessionsAccentSubtleBrush"));
                TextBlock label;
                label.Text(tag);
                label.FontSize(11);
                label.Foreground(ThemeBrush(L"AccentTextFillColorPrimaryBrush"));
                chip.Child(label);
                chips.Children().Append(chip);
            }
            grid.Children().Append(chips);
        }
        return grid;
    }

    // The tool's own logo. Command-line sessions get a terminal chip on the corner.
    FrameworkElement Badge(Tool tool) {
        constexpr double size = 32;
        Grid frame;
        frame.Width(size);
        frame.Height(size);
        frame.VerticalAlignment(VerticalAlignment::Top);
        auto logo = [&](Imaging::BitmapImage const& source, double imageSize) -> UIElement {
            Image image;
            image.Source(source);
            image.Width(imageSize);
            image.Height(imageSize);
            return image;
        };
        const bool cli = tool == Tool::ClaudeCli || tool == Tool::CodexCli;
        const bool codex = tool == Tool::CodexDesktop || tool == Tool::CodexCli;
        auto source = codex ? chatgptLogo : tool == Tool::ClaudeCli && claudeCliLogo ? claudeCliLogo : claudeLogo;
        if (source && codex) {
            // ChatGPT's white knot on its black tile.
            Border tile;
            tile.CornerRadius({8, 8, 8, 8});
            tile.Background(SolidColorBrush(winrt::Windows::UI::Color{255, 0, 0, 0}));
            tile.Child(logo(source, 21));
            frame.Children().Append(tile);
        } else if (source) {
            frame.Children().Append(logo(source, size));
        } else {
            auto look = LookOf(tool);
            Border tile;
            tile.CornerRadius({8, 8, 8, 8});
            tile.Background(SolidColorBrush(look.color));
            FontIcon glyph;
            glyph.Glyph(look.glyph);
            glyph.FontSize(14);
            glyph.Foreground(SolidColorBrush(winrt::Windows::UI::Color{255, 255, 255, 255}));
            tile.Child(glyph);
            frame.Children().Append(tile);
        }
        if (cli) {
            Border chip;
            chip.Width(18);
            chip.Height(13);
            chip.CornerRadius({3, 3, 3, 3});
            chip.HorizontalAlignment(HorizontalAlignment::Right);
            chip.VerticalAlignment(VerticalAlignment::Bottom);
            chip.Margin({0, 0, -5, -4});
            chip.Background(SolidColorBrush(winrt::Windows::UI::Color{255, 0x1E, 0x1E, 0x1E}));
            chip.BorderBrush(SolidColorBrush(winrt::Windows::UI::Color{255, 255, 255, 255}));
            chip.BorderThickness({1, 1, 1, 1});
            TextBlock prompt;
            prompt.Text(L">_");
            prompt.FontFamily(FontFamily(L"Cascadia Mono, Consolas"));
            prompt.FontSize(8);
            prompt.FontWeight(Microsoft::UI::Text::FontWeights::Bold());
            prompt.Foreground(SolidColorBrush(winrt::Windows::UI::Color{255, 255, 255, 255}));
            prompt.HorizontalAlignment(HorizontalAlignment::Center);
            prompt.VerticalAlignment(VerticalAlignment::Center);
            chip.Child(prompt);
            frame.Children().Append(chip);
        }
        return frame;
    }

    TextBlock SnippetBlock(std::wstring const& snippet) {
        TextBlock block;
        block.FontSize(12);
        block.TextWrapping(TextWrapping::Wrap);
        block.MaxLines(3);
        block.TextTrimming(TextTrimming::CharacterEllipsis);
        block.Foreground(ThemeBrush(L"TextFillColorSecondaryBrush"));
        std::wstring run;
        bool highlight = false;
        auto flush = [&] {
            if (run.empty()) return;
            Documents::Run part;
            part.Text(run);
            if (highlight) {
                part.FontWeight(Microsoft::UI::Text::FontWeights::SemiBold());
                part.Foreground(ThemeBrush(L"AccentTextFillColorPrimaryBrush"));
            }
            block.Inlines().Append(part);
            run.clear();
        };
        for (wchar_t c : snippet) {
            if (c == L'\x03') {
                // Found by meaning rather than by the words typed.
                Documents::Run label;
                label.Text(L"\u2248 similar meaning  ");
                label.FontWeight(Microsoft::UI::Text::FontWeights::SemiBold());
                label.Foreground(ThemeBrush(L"AccentTextFillColorPrimaryBrush"));
                block.Inlines().Append(label);
                continue;
            }
            if (c == L'\x01' || c == L'\x02') {
                flush();
                highlight = c == L'\x01';
            } else {
                run.push_back(c == L'\n' || c == L'\r' || c == L'\t' ? L' ' : c);
            }
        }
        flush();
        return block;
    }

    // ---- Actions ----------------------------------------------------------

    sessions::SessionRow const* Row(int64_t id) {
        auto it = rowById.find(id);
        return it == rowById.end() ? nullptr : &sessionRows[it->second];
    }

    void Open(int64_t id) {
        auto row = Row(id);
        if (!row) return;
        if (row->tool == Tool::ClaudeDesktop) {
            // Claude can only open sessions of the organization it is showing;
            // otherwise its link quietly starts a new session.
            std::string active;
            try { active = index->MetaValue("claude_desktop_account"); } catch (...) {}
            if (!active.empty() && active != row->account) {
                ConfirmOtherOrganization(id, AccountName(row->account), AccountName(active));
                return;
            }
        }
        if (!sessions::OpenSession(*row)) ShowProblem(L"Could not open that session in " + std::wstring(sessions::ToolName(row->tool)));
    }

    fire_and_forget ConfirmOtherOrganization(int64_t id, std::wstring target, std::wstring active) {
        auto lifetime = get_strong();
        ContentDialog dialog;
        dialog.XamlRoot(shell.XamlRoot());
        dialog.Title(box_value(L"Switch Claude to " + target));
        TextBlock text;
        text.TextWrapping(TextWrapping::Wrap);
        text.Text(L"This session belongs to " + target + L". Claude seems to be showing " + active +
                  L", and it can only open sessions of the organization it is showing. Switch organizations in Claude "
                  L"(your name in the lower left), then open the session.");
        dialog.Content(text);
        dialog.PrimaryButtonText(L"Open Claude");
        dialog.CloseButtonText(L"Cancel");
        dialog.DefaultButton(ContentDialogButton::Primary);
        auto result = co_await dialog.ShowAsync();
        if (result != ContentDialogResult::Primary || closing) co_return;
        if (auto row = Row(id)) sessions::OpenSession(*row);
    }

    int64_t MenuTarget() {
        if (auto target = cardMenu.Target().try_as<FrameworkElement>())
            if (auto tag = target.Tag()) return unbox_value<int64_t>(tag);
        return 0;
    }

    void BuildCardMenu() {
        cardMenu = MenuFlyout();
        cardMenu.Opening([this](auto&&, auto&&) {
            auto id = MenuTarget();
            auto row = Row(id);
            cardMenu.Items().Clear();
            if (!row) return;
            auto add = [&](wchar_t const* label, wchar_t const* glyph, std::function<void()> action) {
                MenuFlyoutItem item;
                item.Text(label);
                FontIcon icon;
                icon.Glyph(glyph);
                item.Icon(icon);
                item.Click([action](auto&&, auto&&) { action(); });
                cardMenu.Items().Append(item);
            };
            add(L"Open", L"", [this, id] { Open(id); });
            add(L"Add tag…", L"", [this, id] { AskForTag(id); });
            for (auto const& tag : row->tags) {
                std::wstring label = L"Remove tag “" + tag + L"”";
                auto key = row->key;
                add(label.c_str(), L"", [this, key, tag] {
                    index->RemoveTag(key, tag);
                    Reload();
                });
            }
            cardMenu.Items().Append(MenuFlyoutSeparator());
            auto externalId = row->externalId;
            add(L"Copy session id", L"", [externalId] { CopyText(externalId); });
            auto path = row->path;
            add(L"Show transcript file", L"", [path] { sessions::RevealFile(path); });
        });
    }

    static void CopyText(std::wstring const& text) {
        winrt::Windows::ApplicationModel::DataTransfer::DataPackage package;
        package.SetText(text);
        winrt::Windows::ApplicationModel::DataTransfer::Clipboard::SetContent(package);
    }

    fire_and_forget AskForTag(int64_t id) {
        auto lifetime = get_strong();
        auto row = Row(id);
        if (!row) co_return;
        std::string key = row->key;
        TextBox box;
        box.PlaceholderText(L"Tag name");
        ContentDialog dialog;
        dialog.XamlRoot(shell.XamlRoot());
        dialog.Title(box_value(L"Add a tag"));
        dialog.Content(box);
        dialog.PrimaryButtonText(L"Add");
        dialog.CloseButtonText(L"Cancel");
        dialog.DefaultButton(ContentDialogButton::Primary);
        auto result = co_await dialog.ShowAsync();
        std::wstring tag = box.Text().c_str();
        while (!tag.empty() && iswspace(tag.back())) tag.pop_back();
        while (!tag.empty() && iswspace(tag.front())) tag.erase(tag.begin());
        if (result == ContentDialogResult::Primary && !tag.empty() && !closing) {
            index->AddTag(key, tag);
            Reload();
        }
    }

    // ---- Status and settings ---------------------------------------------

    void ShowStatus(sessions::IndexStatus const& status) {
        using State = sessions::IndexStatus::State;
        const bool working = status.state == State::Scanning || status.state == State::Indexing;
        busy.IsActive(working);
        busy.Visibility(working ? Visibility::Visible : Visibility::Collapsed);
        doneIcon.Visibility(status.state == State::Ready ? Visibility::Visible : Visibility::Collapsed);
        problemIcon.Visibility(status.state == State::Failed ? Visibility::Visible : Visibility::Collapsed);
        statusText.Text(status.message);
        ToolTipService::SetToolTip(statusText, box_value(status.message));
        if (status.dataChanged) {
            Reload();
            if (!activeQuery.empty() && status.state == State::Ready) {
                activeQuery.clear();  // refresh results against the new data
                StartSearch();
            }
        }
    }

    void ShowProblem(std::wstring const& message) {
        statusText.Text(message);
        doneIcon.Visibility(Visibility::Collapsed);
        problemIcon.Visibility(Visibility::Visible);
    }

    void ToggleSettings() {
        if (split.IsPaneOpen()) {
            split.IsPaneOpen(false);
            return;
        }
        BuildSettings();
        split.IsPaneOpen(true);
    }

    void BuildSettings() {
        accountsPanel.Children().Clear();
        for (auto const& account : accountRows) {
            Border card;
            card.CornerRadius({8, 8, 8, 8});
            card.Padding({12, 10, 12, 12});
            card.Background(ThemeBrush(L"CardBackgroundFillColorDefaultBrush"));
            StackPanel stack;
            stack.Spacing(6);
            TextBlock heading;
            heading.Text((account.family == sessions::Family::Codex ? L"ChatGPT / Codex  ·  " : L"Claude  ·  ") +
                         Number(account.sessions) + (account.sessions == 1 ? L" session" : L" sessions"));
            heading.FontSize(12);
            heading.Foreground(ThemeBrush(L"TextFillColorSecondaryBrush"));
            stack.Children().Append(heading);
            TextBox name;
            name.Text(account.named ? account.name : L"");
            name.PlaceholderText(account.name);
            Automation::AutomationProperties::SetName(name, L"Account name");
            auto id = account.id;
            name.LostFocus([this, id, name](auto&&, auto&&) { Rename(id, name.Text().c_str()); });
            name.KeyDown([this, id, name](auto&&, Input::KeyRoutedEventArgs const& e) {
                if (e.Key() == winrt::Windows::System::VirtualKey::Enter) Rename(id, name.Text().c_str());
            });
            stack.Children().Append(name);
            Grid footer;
            TextBlock idText;
            idText.Text(sessions::Wide(id));
            idText.FontSize(11);
            idText.Foreground(ThemeBrush(L"TextFillColorTertiaryBrush"));
            idText.VerticalAlignment(VerticalAlignment::Center);
            idText.IsTextSelectionEnabled(true);
            idText.TextTrimming(TextTrimming::CharacterEllipsis);
            idText.Margin({0, 0, 90, 0});
            footer.Children().Append(idText);
            Button rebuild;
            rebuild.Content(box_value(L"Rebuild"));
            rebuild.HorizontalAlignment(HorizontalAlignment::Right);
            rebuild.Click([this, id](auto&&, auto&&) {
                if (indexer) indexer->RebuildAccount(id);
            });
            footer.Children().Append(rebuild);
            stack.Children().Append(footer);
            card.Child(stack);
            accountsPanel.Children().Append(card);
        }
        try {
            auto totals = index->Count();
            std::error_code error;
            auto size = std::filesystem::file_size(databaseFile, error);
            indexInfo.Text(databaseFile.wstring() + L"\n" + Number(totals.sessions) + L" sessions, " + Number(totals.messages) +
                           L" messages, " + Number(int64_t(size / (1024 * 1024))) + L" MB. " + Number(totals.skipped) +
                           L" helper or empty transcripts are skipped.");
        } catch (...) {
        }
    }

    void Rename(std::string const& id, std::wstring name) {
        while (!name.empty() && iswspace(name.back())) name.pop_back();
        auto current = accountNames.find(id);
        bool named = false;
        for (auto const& account : accountRows)
            if (account.id == id) named = account.named;
        if (current != accountNames.end() && named && current->second == name) return;
        if (!named && name.empty()) return;
        index->RenameAccount(id, name);
        bool open = split.IsPaneOpen();
        Reload();
        if (open) split.IsPaneOpen(true);
    }
};

namespace {

// Command-line test mode: `Sessions.exe --index [--search "words"] [--db file]`
// runs one catch-up scan without a window and prints a summary.
int RunHeadless(int argc, wchar_t** argv) {
    std::filesystem::path database = sessions::Index::DefaultFile();
    std::vector<std::wstring> queries;
    bool scan = false;
    for (int i = 1; i < argc; ++i) {
        std::wstring_view arg(argv[i]);
        if (arg == L"--index") scan = true;
        else if (arg == L"--db" && i + 1 < argc) database = argv[++i];
        else if (arg == L"--search" && i + 1 < argc) queries.push_back(argv[++i]);
    }
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (!out || out == INVALID_HANDLE_VALUE) {
        AttachConsole(ATTACH_PARENT_PROCESS);
        out = GetStdHandle(STD_OUTPUT_HANDLE);
    }
    auto print = [out](std::wstring const& text) {
        auto bytes = sessions::Utf8(text + L"\n");
        DWORD written{};
        WriteFile(out, bytes.data(), DWORD(bytes.size()), &written, nullptr);
    };
    try {
        if (scan) {
            auto started = sessions::NowMs();
            int lastShown = -1;
            auto status = sessions::Indexer::RunOnce(database, [&](sessions::IndexStatus const& s) {
                if (s.state == sessions::IndexStatus::State::Indexing && s.done / 100 != lastShown) {
                    lastShown = s.done / 100;
                    print(s.message);
                }
            });
            print(status.message + L" (" + std::to_wstring((sessions::NowMs() - started) / 1000) + L" s)");
        }
        sessions::Index index(database);
        auto rows = index.LoadSessions();
        std::unordered_map<int64_t, sessions::SessionRow const*> byId;
        std::unordered_map<int, int> perTool;
        for (auto const& row : rows) {
            byId[row.id] = &row;
            ++perTool[int(row.tool)];
        }
        for (auto tool : {Tool::ClaudeDesktop, Tool::ClaudeCli, Tool::CodexDesktop, Tool::CodexCli})
            print(std::wstring(sessions::ToolName(tool)) + L": " + std::to_wstring(perTool[int(tool)]));
        for (auto const& account : index.Accounts())
            print(L"account " + sessions::Wide(account.id) + L" (" + account.name + L"): " + std::to_wstring(account.sessions));
        auto totals = index.Count();
        print(L"sessions " + std::to_wstring(totals.sessions) + L", skipped " + std::to_wstring(totals.skipped) + L", messages " +
              std::to_wstring(totals.messages));
        std::wstring problem;
        auto embedder = sessions::Embedder::Shared(&problem);
        print(embedder ? L"meaning search on " + embedder->Device() : problem);
        sessions::HybridSearch hybrid(database);
        for (auto const& query : queries) {
            auto started = sessions::NowMs();
            auto hits = hybrid.Run(query, 8);
            print(L"search \u201C" + query + L"\u201D: " + std::to_wstring(hits.size()) + L" sessions in " +
                  std::to_wstring(sessions::NowMs() - started) + L" ms");
            for (size_t i = 0; i < hits.size() && i < 8; ++i) {
                auto row = byId[hits[i].session];
                auto snippet = (hits[i].byMeaning ? L"[meaning " + std::to_wstring(int(hits[i].similarity * 100)) + L"] " : std::wstring()) +
                               hits[i].snippet;
                std::replace(snippet.begin(), snippet.end(), L'\x01', L'[');
                std::replace(snippet.begin(), snippet.end(), L'\x02', L']');
                std::replace(snippet.begin(), snippet.end(), L'\n', L' ');
                print(L"  " + std::wstring(row ? sessions::ToolName(row->tool) : L"?") + L" | " + (row ? row->title : L"?") + L" | " + snippet);
            }
        }
    } catch (std::exception const& e) {
        print(L"error: " + sessions::Wide(e.what()));
        return 1;
    }
    return 0;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    int argc{};
    auto argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 1; argv && i < argc; ++i) {
        std::wstring_view arg(argv[i]);
        if (arg == L"--index" || arg == L"--search") {
            int code = RunHeadless(argc, argv);
            LocalFree(argv);
            return code;
        }
    }
    if (argv) LocalFree(argv);
    try {
        init_apartment(apartment_type::single_threaded);
        Application::Start([](auto&&) { make<App>(); });
    } catch (hresult_error const& e) {
        MessageBoxW(nullptr, e.message().c_str(), L"Sessions could not start", MB_OK | MB_ICONERROR);
        return 1;
    }
    return 0;
}
