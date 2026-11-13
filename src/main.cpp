#include <windows.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <shcore.h>
#include <commdlg.h>
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
#include <microsoft.ui.xaml.window.h>
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
#include "ChatGptImport.h"
#include "Text.h"
#include "TrayIcon.h"
#include "WebSync.h"

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
    case Tool::ClaudeWeb: return {Rgb(0xD9, 0x77, 0x57), L""};
    case Tool::ChatGptWeb: return {Rgb(0x10, 0xA3, 0x7F), L""};
    case Tool::Gemini: return {Rgb(0x42, 0x85, 0xF4), L""};
    case Tool::AntigravityDesktop: return {Rgb(0x20, 0x21, 0x24), L""};
    case Tool::AntigravityCli: return {Rgb(0x20, 0x21, 0x24), L""};
    }
    return {Rgb(0x60, 0x60, 0x60), L""};
}

using sessions::Found;

// A PNG embedded in the executable as an image source; nullptr when missing.
// `pixels` is the exact number of screen pixels it will cover (its size in
// layout units times the display scale). The large source is scaled down once
// to that size, so it is drawn 1:1: neither shrunk at draw time (jagged) nor
// decoded too small and stretched (blurry).
Imaging::BitmapImage EmbeddedImage(int id, int pixels = 0) {
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
    if (pixels > 0) {
        image.DecodePixelType(Imaging::DecodePixelType::Physical);
        image.DecodePixelWidth(pixels);
        image.DecodePixelHeight(pixels);
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
      <Button x:Name='SyncNow' Width='32' Height='32' Padding='0' Background='Transparent' BorderThickness='0' AutomationProperties.Name='Check for new sessions'
              ToolTipService.ToolTip='Check every source for new sessions now'>
        <FontIcon Glyph='&#xE895;' FontSize='14'/>
      </Button>
      <Button x:Name='Gear' Width='32' Height='32' Padding='0' Background='Transparent' BorderThickness='0' AutomationProperties.Name='Settings' ToolTipService.ToolTip='Settings'>
        <FontIcon Glyph='&#xE713;' FontSize='14'/>
      </Button>
    </StackPanel>
  </Grid>
  <SplitView x:Name='Split' Grid.Row='1' PanePlacement='Right' DisplayMode='Overlay' OpenPaneLength='480' IsPaneOpen='False'
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
          <TextBlock Text='Web chats' FontWeight='SemiBold' Margin='0,8,0,0'/>
          <TextBlock TextWrapping='Wrap' FontSize='12' Foreground='{ThemeResource TextFillColorSecondaryBrush}'
                     Text='Sign in once per site. A window opens on the site; sign in as usual and it closes by itself, then the chats come in. Sessions never sees your password. Your chats stay in sync every 30 minutes while Sessions is open.'/>
          <StackPanel x:Name='WebSites' Spacing='8'/>
          <HyperlinkButton x:Name='SyncWeb' Content='Check all sites again now' Padding='0,2'/>
          <TextBlock TextWrapping='Wrap' FontSize='12' Foreground='{ThemeResource TextFillColorSecondaryBrush}' Margin='0,4,0,0'
                     Text='ChatGPT lets Sessions open only so many chats an hour, so a long history fills in slowly. To load it all at once, ask ChatGPT for your data (Settings, Data controls, Export data), then choose the zip it emails you.'/>
          <StackPanel Spacing='6'>
            <Button x:Name='ImportChatGpt' Content='Load a ChatGPT export'/>
            <TextBlock x:Name='ImportInfo' FontSize='12' TextWrapping='Wrap'
                       Foreground='{ThemeResource TextFillColorSecondaryBrush}'/>
          </StackPanel>
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
      <TextBox x:Name='Search' Margin='24,4,24,10' PlaceholderText='Search what you or the AI said' AutomationProperties.Name='Search sessions' IsSpellCheckEnabled='False'/>
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
        <GridView.ItemTemplate>
          <DataTemplate><Border/></DataTemplate>
        </GridView.ItemTemplate>
      </GridView>
      <TextBlock x:Name='Empty' Grid.Row='2' HorizontalAlignment='Center' VerticalAlignment='Center' Visibility='Collapsed'
                 Foreground='{ThemeResource TextFillColorSecondaryBrush}' TextWrapping='Wrap' TextAlignment='Center' MaxWidth='420'/>
    </Grid>
  </SplitView>
</Grid>)";

}  // namespace


// One line per site for Settings, in plain words: who is signed in and how many
// chats Sessions has, or what to do. With `details`, the sites' own answers too
// (shown on hover).
std::wstring WebSummary(sessions::WebSync& sync, bool details = false) {
    std::wstring out;
    for (auto const& site : sync.Statuses()) {
        std::wstring name = site.tool == Tool::ClaudeWeb ? L"Claude" : sessions::ToolName(site.tool);
        std::wstring chats = std::to_wstring(site.chats) + (site.chats == 1 ? L" chat" : L" chats") + L" in Sessions";
        std::wstring line;
        if (!site.checked) {
            line = L"\u2013  " + name + L": not checked yet.";
        } else if (site.signedIn) {
            line = L"\u2713  " + name + L": signed in as " + (site.who.empty() ? std::wstring(L"your account") : site.who) + L". " + chats + L".";
            if (!site.problem.empty()) line += L" Some chats could not be read; Sessions will try again.";
        } else {
            bool reached = site.problem.find(L"could not reach") == std::wstring::npos && site.problem.find(L"stopped answering") == std::wstring::npos;
            line = L"\u2717  " + name + (reached ? L": signed out. Press " + name + L" above to sign in." : L": could not reach the site; Sessions will try again.");
            if (site.chats) line += L" " + chats + L" are kept.";
        }
        if (site.checkedAt > 0) line += L" (Checked " + FriendlyDate(site.checkedAt) + L")";
        if (details && !site.problem.empty()) line += L"\n      " + site.problem;
        out += line + L"\n";
    }
    if (!out.empty()) out.pop_back();
    return out;
}

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
    Imaging::BitmapImage claudeLogo{nullptr}, claudeCliLogo{nullptr}, chatgptLogo{nullptr}, geminiLogo{nullptr}, antigravityLogo{nullptr};
    Collections::IObservableVector<IInspectable> items{nullptr};
    Microsoft::UI::Dispatching::DispatcherQueueTimer searchDelay{nullptr};

    std::filesystem::path databaseFile;
    std::unique_ptr<sessions::Index> index;
    std::unique_ptr<sessions::Indexer> indexer;
    std::unique_ptr<Searcher> searcher;
    std::unique_ptr<sessions::WebSync> webSync;
    std::unique_ptr<sessions::WebSync> webSync2;  // second ChatGPT account
    // The second slot syncs only once it has been signed in to.
    bool SecondChatGptUsed() { for (auto const& s : webSync2->Statuses()) if (s.checked) return true; return false; }
    Microsoft::UI::Dispatching::DispatcherQueueTimer webTimer{nullptr};
    std::wstring indexMessage, webMessage, webMessage2;
    StackPanel webSites{nullptr};
    std::vector<Tool> toolChoices;  // parallel to ToolFilter items after "All tools"

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
    bool startInTray = false, startQuick = false;
    int64_t quickShownAt = 0;

    // Tray icon and the full-screen quick search it opens.
    sessions::TrayIcon tray;
    Window quick{nullptr};
    FrameworkElement quickRoot{nullptr};
    TextBox quickBox{nullptr};
    ListView quickList{nullptr};
    TextBlock quickHint{nullptr};
    Collections::IObservableVector<IInspectable> quickItems{nullptr};
    std::unique_ptr<Searcher> quickSearcher;
    Microsoft::UI::Dispatching::DispatcherQueueTimer quickDelay{nullptr};
    uint64_t quickTicket = 0;
    bool quickOpening = false;
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

        LoadLogos();

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
            if (auto id = e.ClickedItem().try_as<int64_t>()) Open(*id);
        });
        // Only the cards on screen exist: the list holds session ids and each card is
        // made as its slot comes into view (thousands of sessions stay quick).
        cards.ContainerContentChanging([this](ListViewBase const&, ContainerContentChangingEventArgs const& args) {
            args.Handled(true);
            auto slot = args.ItemContainer().ContentTemplateRoot().try_as<Border>();
            if (!slot) return;
            if (args.InRecycleQueue()) {
                slot.Child(nullptr);
                return;
            }
            auto id = args.Item().try_as<int64_t>();
            auto row = id ? Row(*id) : nullptr;
            try {
                slot.Child(row ? CardFor(*row).as<UIElement>() : nullptr);
            } catch (winrt::hresult_error const&) {
            }
        });
        BuildCardMenu();

        toolFilter.Items().Append(box_value(L"All tools"));
        for (auto tool : {Tool::ClaudeDesktop, Tool::ClaudeCli, Tool::CodexDesktop, Tool::CodexCli, Tool::AntigravityDesktop, Tool::AntigravityCli, Tool::ClaudeWeb, Tool::ChatGptWeb,
                          Tool::Gemini}) {
            toolChoices.push_back(tool);
            toolFilter.Items().Append(box_value(sessions::ToolName(tool)));
        }
        toolFilter.SelectedIndex(0);
        for (auto label : {L"Last activity", L"Best match", L"Created", L"Most messages", L"Oldest first"}) sortBox.Items().Append(box_value(label));
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

        // The lightest check of every source: files on this PC, then one small request per site.
        Named<Button>(shell, L"SyncNow").Click([this](auto&&, auto&&) {
            if (indexer) indexer->Rescan();
            if (webSync) webSync->SyncAll(true);
            if (webSync2 && SecondChatGptUsed()) webSync2->SyncAll(true);
        });
        Named<Button>(shell, L"Gear").Click([this](auto&&, auto&&) { ToggleSettings(); });
        Named<Button>(shell, L"CloseSettings").Click([this](auto&&, auto&&) { split.IsPaneOpen(false); });
        Named<Button>(shell, L"RebuildAll").Click([this](auto&&, auto&&) {
            if (indexer) indexer->RebuildAll();
            split.IsPaneOpen(false);
        });
        Named<Button>(shell, L"ImportChatGpt").Click([this](auto&&, auto&&) { ImportChatGpt(); });
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
        RestorePlacement();
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
            SavePlacement();
            closing = true;
            searchDelay.Stop();
            if (quickDelay) quickDelay.Stop();
            if (webTimer) webTimer.Stop();
            webSync2.reset();
            webSync.reset();
            if (indexer) indexer->Stop();
            indexer.reset();
            searcher.reset();
            quickSearcher.reset();
            tray.Remove();
            if (quick) quick.Close();
            appIcon.Reset();
            Application::Current().Exit();
        });
        // Minimizing puts Sessions in the notification area instead of the taskbar.
        window.AppWindow().Changed([this](AppWindow const& sender, AppWindowChangedEventArgs const& args) {
            if (!args.DidPresenterChange() && !args.DidSizeChange()) return;
            auto presenter = sender.Presenter().try_as<OverlappedPresenter>();
            if (presenter && presenter.State() == OverlappedPresenterState::Minimized) {
                SavePlacement();
                sender.Hide();
            }
        });
        tray.Create([this] { ShowQuick(); }, [this] { ShowMain(); });
        shell.Loaded([this](auto&&, auto&&) {
            auto check = [this] {
                double before = logoScale;
                LoadLogos();
                if (before != logoScale) {
                    shown.clear();
                    Render();
                }
            };
            check();
            shell.XamlRoot().Changed([check](auto&&, auto&&) { check(); });
        });
        if (!startInTray) {
            window.Activate();
            search.Focus(FocusState::Programmatic);
        }
        if (startQuick) ShowQuick();
        indexer->Start();

        webSites = Named<StackPanel>(shell, L"WebSites");
        webSync = std::make_unique<sessions::WebSync>(*index, [this](std::wstring const& status, bool changed, bool finished) {
            if (closing) return;
            // Row-only updates carry no status text; keep the last one until the run ends.
            if (finished) webMessage.clear();
            else if (!status.empty()) webMessage = status;
            ShowCombinedStatus();
            if (split && split.IsPaneOpen()) ShowWebInfo();
            if (changed) Reload();
            if (finished) {
                if (changed && indexer) indexer->Rescan();  // learn the meaning of new web messages
                ShowWebInfo();
            }
        });
        // A second ChatGPT sign-in (say, a work account) in its own browser profile.
        webSync2 = std::make_unique<sessions::WebSync>(
            *index,
            [this](std::wstring const& status, bool changed, bool finished) {
                if (closing) return;
                if (finished) webMessage2.clear();
                else if (!status.empty()) webMessage2 = status;
                ShowCombinedStatus();
                if (split && split.IsPaneOpen()) ShowWebInfo();
                if (changed) Reload();
                if (finished) {
                    if (changed && indexer) indexer->Rescan();
                    ShowWebInfo();
                }
            },
            sessions::ExecutableFolder() / L"Browser2", 2);
        Named<HyperlinkButton>(shell, L"SyncWeb").Click([this](auto&&, auto&&) {
            webSync->SyncAll(true);
            if (SecondChatGptUsed()) webSync2->SyncAll(true);
        });
        webTimer = window.DispatcherQueue().CreateTimer();
        webTimer.Interval(std::chrono::seconds(20));
        webTimer.IsRepeating(true);
        webTimer.Tick([this](auto&&, auto&&) {
            webTimer.Interval(std::chrono::minutes(30));
            if (webSync && !closing) webSync->SyncAll();
            if (webSync2 && !closing && SecondChatGptUsed()) webSync2->SyncAll();
        });
        webTimer.Start();
    }

    // ---- ChatGPT export -------------------------------------------------------

    std::thread importer;
    void ImportChatGpt() {
        if (importer.joinable()) return;  // one at a time
        HWND handle{};
        window.as<IWindowNative>()->get_WindowHandle(&handle);
        wchar_t file[MAX_PATH * 4] = L"";
        OPENFILENAMEW dialog{sizeof(dialog)};
        dialog.hwndOwner = handle;
        dialog.lpstrFilter = L"ChatGPT export (*.zip; conversations.json)\0*.zip;*.json\0All files\0*.*\0";
        dialog.lpstrFile = file;
        dialog.nMaxFile = DWORD(std::size(file));
        dialog.lpstrTitle = L"Choose the ChatGPT export zip";
        dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
        if (!GetOpenFileNameW(&dialog)) return;
        auto info = Named<TextBlock>(shell, L"ImportInfo");
        auto button = Named<Button>(shell, L"ImportChatGpt");
        info.Text(L"Reading the export\u2026");
        button.IsEnabled(false);
        auto queue = window.DispatcherQueue();
        auto weak = get_weak();
        std::filesystem::path source = file;
        auto say = [queue, weak](std::wstring text) {
            queue.TryEnqueue([weak, text] {
                if (auto app = weak.get(); app && !app->closing) {
                    Named<TextBlock>(app->shell, L"ImportInfo").Text(text);
                    app->webMessage = text;
                    app->ShowCombinedStatus();
                }
            });
        };
        importer = std::thread([this, queue, weak, source, say] {
            sessions::ImportResult result;
            try {
                result = sessions::ImportChatGptExport(databaseFile, source, say);
            } catch (std::exception const& e) {
                result.problem = L"Could not load it: " + sessions::Wide(e.what());
            }
            queue.TryEnqueue([weak, result] {
                auto app = weak.get();
                if (!app || app->closing) return;
                app->importer.join();
                std::wstring text = result.conversations
                                        ? L"Loaded " + Number(result.conversations) + L" ChatGPT chats from the export."
                                        : result.problem;
                Named<TextBlock>(app->shell, L"ImportInfo").Text(text);
                Named<Button>(app->shell, L"ImportChatGpt").IsEnabled(true);
                app->webMessage.clear();
                app->ShowCombinedStatus();
                app->Reload();
                if (app->indexer) app->indexer->Rescan();  // learn the meaning of the new messages
            });
        });
    }

    // ---- Window placement ---------------------------------------------------

    // Opens where it was last closed, at the same size (and maximized if it was).
    // The first time, or if that spot is no longer on any screen: centered on the
    // screen under the pointer, wide enough for four columns of cards.
    void RestorePlacement() {
        auto appWindow = window.AppWindow();
        std::string saved;
        try { saved = index->MetaValue("window_placement"); } catch (...) {}
        int x{}, y{}, w{}, h{}, maximized{};
        if (sscanf_s(saved.c_str(), "%d,%d,%d,%d,%d", &x, &y, &w, &h, &maximized) == 5 && w > 200 && h > 200) {
            auto area = DisplayArea::GetFromRect({x, y, w, h}, DisplayAreaFallback::None);
            if (area) {
                auto work = area.WorkArea();
                // At least a title bar's worth must be visible.
                bool visible = x + w > work.X + 100 && x < work.X + work.Width - 100 && y >= work.Y - 10 && y < work.Y + work.Height - 60;
                if (visible) {
                    appWindow.MoveAndResize({x, y, w, h});
                    if (maximized)
                        if (auto presenter = appWindow.Presenter().try_as<OverlappedPresenter>()) presenter.Maximize();
                    return;
                }
            }
        }
        POINT cursor{};
        GetCursorPos(&cursor);
        auto area = DisplayArea::GetFromPoint({cursor.x, cursor.y}, DisplayAreaFallback::Primary);
        auto work = area.WorkArea();
        HWND handle{};
        window.as<IWindowNative>()->get_WindowHandle(&handle);
        const double scale = (handle ? GetDpiForWindow(handle) : GetDpiForSystem()) / 96.0;
        // Four 300-wide cards with their gaps, the page padding and a scroll bar.
        int width = std::min(int(1330 * scale), work.Width - int(40 * scale));
        int height = std::min(int(900 * scale), work.Height - int(40 * scale));
        appWindow.MoveAndResize({work.X + (work.Width - width) / 2, work.Y + (work.Height - height) / 2, width, height});
    }

    void SavePlacement() {
        if (!window || !index) return;
        auto appWindow = window.AppWindow();
        auto presenter = appWindow.Presenter().try_as<OverlappedPresenter>();
        if (!presenter) return;
        int maximized = presenter.State() == OverlappedPresenterState::Maximized ? 1 : 0;
        // The restored (not maximized) rectangle, so un-maximizing later returns to it.
        HWND handle{};
        window.as<IWindowNative>()->get_WindowHandle(&handle);
        WINDOWPLACEMENT placement{sizeof(placement)};
        RECT r{};
        if (handle && GetWindowPlacement(handle, &placement)) {
            r = placement.rcNormalPosition;  // workspace coordinates; equal to screen coordinates without a top/left taskbar
            if (presenter.State() == OverlappedPresenterState::Minimized) maximized = (placement.flags & WPF_RESTORETOMAXIMIZED) ? 1 : 0;
        } else {
            auto position = appWindow.Position();
            auto size = appWindow.Size();
            r = {position.X, position.Y, position.X + size.Width, position.Y + size.Height};
        }
        char text[96];
        sprintf_s(text, "%ld,%ld,%ld,%ld,%d", r.left, r.top, r.right - r.left, r.bottom - r.top, maximized);
        try { index->SetMetaValue("window_placement", text); } catch (...) {}
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
            // Names alone don't say the site ("Personal" is ChatGPT, "Personal Ultra" is Claude).
            std::wstring site = account.id.rfind("gemini:", 0) == 0        ? L"Gemini"
                                : account.id.rfind("antigravity:", 0) == 0 ? L"Antigravity"
                                : account.family == sessions::Family::Codex ? L"ChatGPT"
                                                                             : L"Claude";
            accountFilter.Items().Append(box_value(account.name.rfind(site, 0) == 0 ? account.name : site + L" \u00B7 " + account.name));
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
            sortChosen = false;
            SetSort(0);
            Render();
            return;
        }
        if (query == activeQuery && !searching) return;
        if (activeQuery.empty()) sortChosen = false;  // a fresh search starts on Best match
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
        if (int tool = toolFilter.SelectedIndex(); tool > 0 && size_t(tool - 1) < toolChoices.size() && row.tool != toolChoices[size_t(tool - 1)])
            return false;
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
        else if (sort == 4)
            std::stable_sort(visible.begin(), visible.end(), [](auto a, auto b) { return a->lastActivity < b->lastActivity; });
        else
            std::stable_sort(visible.begin(), visible.end(), byActivity);

        std::vector<int64_t> ids;
        std::vector<IInspectable> elements;
        ids.reserve(visible.size());
        elements.reserve(visible.size());
        for (auto row : visible) {
            ids.push_back(row->id);
            elements.push_back(box_value(row->id));
        }
        if (ids != shown) {
            // A new search or filter starts at the top; a live update keeps its place.
            bool newList = shown.empty() || ids.empty() || ids.front() != shown.front();
            items.ReplaceAll(elements);
            shown = std::move(ids);
            if (newList && items.Size() > 0) cards.ScrollIntoView(items.GetAt(0));
        } else {
            // Same cards in the same order: refresh the ones on screen (new titles, counts, snippets).
            for (uint32_t i = 0; i < items.Size(); ++i)
                if (auto container = cards.ContainerFromIndex(int32_t(i)).try_as<Primitives::SelectorItem>())
                    if (auto slot = container.ContentTemplateRoot().try_as<Border>())
                        if (auto row = Row(shown[i])) {
                            // Leave a card alone when nothing it shows has changed (no flicker).
                            auto current = slot.Child().try_as<FrameworkElement>();
                            auto built = builtSignature.find(row->id);
                            if (current && current.Tag() && unbox_value<int64_t>(current.Tag()) == row->id && built != builtSignature.end() &&
                                built->second == CardSignature(*row))
                                continue;
                            slot.Child(CardFor(*row).as<UIElement>());
                        }
        }

        std::wstring count;
        if (!activeQuery.empty())
            count = Number(int64_t(visible.size())) + (visible.size() == 1 ? L" session matches" : L" sessions match");
        else
            count = Number(int64_t(visible.size())) + (visible.size() == 1 ? L" session" : L" sessions");
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

    std::wstring SnippetFor(sessions::SessionRow const& row) {
        if (!activeQuery.empty())
            if (auto it = found.find(row.id); it != found.end()) return (it->second.byMeaning ? L"\x03" : L"") + it->second.snippet;
        return {};
    }

    // Everything a card shows, as one string: a card is rebuilt only when this changes.
    std::unordered_map<int64_t, std::wstring> builtSignature;
    std::wstring CardSignature(sessions::SessionRow const& row) {
        std::wstring snippet = SnippetFor(row);
        std::wstring signature = row.title + L"\x1F" + row.folder + L"\x1F" + AccountName(row.account) + L"\x1F" + std::to_wstring(row.messages) + L"\x1F" +
                                 std::to_wstring(row.lastActivity) + L"\x1F" + std::to_wstring(int(row.tool)) + L"\x1F" + snippet +
                                 (row.archived ? L"\x1F" L"a" : L"");
        for (auto const& tag : row.tags) signature += L"\x1F#" + tag;
        if (sortBox && sortBox.SelectedIndex() == 2) signature += L"\x1F" L"created";  // cards show the start date
        signature += activeQuery.empty() ? L"\x1F" L"browse" : L"\x1F" L"search";  // card height differs
        return signature;
    }

    IInspectable CardFor(sessions::SessionRow const& row) {
        std::wstring snippet = SnippetFor(row);
        if (builtSignature.size() > 20000) builtSignature.clear();
        builtSignature[row.id] = CardSignature(row);
        Border card;
        card.Tag(box_value(row.id));
        card.CornerRadius({8, 8, 8, 8});
        card.BorderThickness({1, 1, 1, 1});
        card.Padding({14, 12, 14, 12});
        card.Width(300);
        card.Background(ThemeBrush(L"CardBackgroundFillColorDefaultBrush"));
        card.BorderBrush(ThemeBrush(L"CardStrokeColorDefaultBrush"));
        card.ContextFlyout(cardMenu);
        // Uniform tiles; taller while searching to make room for the passage.
        card.Height((activeQuery.empty() ? 104 : 168) + (row.tags.empty() ? 0 : 34));
        card.Child(CardContent(row, snippet));
        Automation::AutomationProperties::SetName(card, hstring(row.title + L", " + sessions::ToolName(row.tool)));
        return card;
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
        std::wstring opens = sessions::OpensInBrowser(row.tool)      ? L"Opens in your browser"
                             : row.tool == Tool::ClaudeWeb                ? L"Opens in the Claude app"
                             : sessions::IsCommandLine(row.tool)          ? L"Resumes in Windows Terminal"
                             : row.tool == Tool::CodexDesktop             ? L"Opens in the Codex app"
                             : row.tool == Tool::AntigravityDesktop       ? L"Opens the Antigravity app (it has no link to a single conversation)"
                                                                          : L"Opens in the Claude app";
        ToolTipService::SetToolTip(badge, box_value(std::wstring(sessions::ToolName(row.tool)) + L"  \u00B7  " + AccountName(row.account) +
                                                    L"\n" + opens));
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
        const bool byCreated = sortBox && sortBox.SelectedIndex() == 2 && row.created > 0;
        std::wstring when = (byCreated ? L"Started " + FriendlyDate(row.created) : FriendlyDate(row.lastActivity)) + L"  \u00B7  " +
                            (row.messages == 0 ? std::wstring(L"name only, messages not read yet")
                                               : Number(row.messages) + (row.messages == 1 ? L" message" : L" messages"));
        if (row.archived) when += L"  \u00B7  archived";
        auto dateLine = line(L"");
        // Date and size first, then the folder, so a long folder name is what gets cut short.
        Documents::Run rest;
        rest.Text(when);
        dateLine.Inlines().Append(rest);
        if (!row.folder.empty()) {
            Documents::Run gap;
            gap.Text(L"  \u00B7  ");
            dateLine.Inlines().Append(gap);
            Documents::Run icon;
            icon.Text(L"\uE8B7 ");
            icon.FontFamily(FontFamily(L"Segoe Fluent Icons"));
            dateLine.Inlines().Append(icon);
            Documents::Run folder;
            folder.Text(row.folder);
            folder.FontWeight(Microsoft::UI::Text::FontWeights::SemiBold());
            dateLine.Inlines().Append(folder);
            ToolTipService::SetToolTip(dateLine, box_value(row.cwd.empty() ? row.folder : row.cwd));
        }
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

    // Decodes the logos for the window's current display scale; again when the
    // window moves to a monitor with a different scale.
    double logoScale = 0;
    void LoadLogos() {
        HWND handle{};
        window.as<IWindowNative>()->get_WindowHandle(&handle);
        double scale = handle ? GetDpiForWindow(handle) / 96.0 : 1.0;
        if (shell.XamlRoot()) scale = shell.XamlRoot().RasterizationScale();
        if (scale == logoScale) return;
        logoScale = scale;
        auto pixels = [scale](double size) { return int(std::lround(size * scale)); };
        Named<Image>(shell, L"BrandIcon").Source(EmbeddedImage(IDR_BRAND, pixels(20)));
        claudeLogo = EmbeddedImage(IDR_LOGO_CLAUDE, pixels(32));
        claudeCliLogo = EmbeddedImage(IDR_LOGO_CLAUDE_CLI, pixels(32));
        chatgptLogo = EmbeddedImage(IDR_LOGO_CHATGPT, pixels(20));
        geminiLogo = EmbeddedImage(IDR_LOGO_GEMINI, pixels(32));
        antigravityLogo = EmbeddedImage(IDR_LOGO_ANTIGRAVITY, pixels(32));
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
        const bool cli = sessions::IsCommandLine(tool);
        const bool web = sessions::OpensInBrowser(tool);
        const bool chat = tool == Tool::ClaudeWeb;
        const bool codex = tool == Tool::CodexDesktop || tool == Tool::CodexCli || tool == Tool::ChatGptWeb;
        const bool antigravity = tool == Tool::AntigravityDesktop || tool == Tool::AntigravityCli;
        auto source = antigravity          ? antigravityLogo
                      : tool == Tool::Gemini ? geminiLogo
                      : codex              ? chatgptLogo
                      : tool == Tool::ClaudeCli && claudeCliLogo ? claudeCliLogo
                                                                   : claudeLogo;
        if (source && codex) {
            // ChatGPT's white knot on its black tile.
            Border tile;
            tile.CornerRadius({8, 8, 8, 8});
            tile.Background(SolidColorBrush(winrt::Windows::UI::Color{255, 0, 0, 0}));
            tile.Child(logo(source, 20));  // even margins keep it on whole pixels
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
        if (web) {
            Border chip;
            chip.Width(15);
            chip.Height(15);
            chip.CornerRadius({8, 8, 8, 8});
            chip.HorizontalAlignment(HorizontalAlignment::Right);
            chip.VerticalAlignment(VerticalAlignment::Bottom);
            chip.Margin({0, 0, -4, -4});
            chip.Background(SolidColorBrush(winrt::Windows::UI::Color{255, 0x1E, 0x1E, 0x1E}));
            chip.BorderBrush(SolidColorBrush(winrt::Windows::UI::Color{255, 255, 255, 255}));
            chip.BorderThickness({1, 1, 1, 1});
            FontIcon globe;
            globe.Glyph(L"\uE774");  // globe: opens in your browser
            globe.FontSize(8);
            globe.Foreground(SolidColorBrush(winrt::Windows::UI::Color{255, 255, 255, 255}));
            chip.Child(globe);
            frame.Children().Append(chip);
        }
        if (chat) {
            Border chip;
            chip.Width(15);
            chip.Height(15);
            chip.CornerRadius({8, 8, 8, 8});
            chip.HorizontalAlignment(HorizontalAlignment::Right);
            chip.VerticalAlignment(VerticalAlignment::Bottom);
            chip.Margin({0, 0, -4, -4});
            chip.Background(SolidColorBrush(winrt::Windows::UI::Color{255, 0x1E, 0x1E, 0x1E}));
            chip.BorderBrush(SolidColorBrush(winrt::Windows::UI::Color{255, 255, 255, 255}));
            chip.BorderThickness({1, 1, 1, 1});
            FontIcon bubble;
            bubble.Glyph(L"\uE8BD");  // speech bubble: a Claude chat, opens in the Claude app
            bubble.FontSize(8);
            bubble.Foreground(SolidColorBrush(winrt::Windows::UI::Color{255, 255, 255, 255}));
            chip.Child(bubble);
            frame.Children().Append(chip);
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
        if (row->tool == Tool::ClaudeDesktop || row->tool == Tool::ClaudeWeb) {
            // Claude can only open sessions of the organization it is showing;
            // otherwise its link quietly starts a new session.
            std::string active;
            try { active = index->MetaValue("claude_desktop_account"); } catch (...) {}
            if (!active.empty() && active != row->account) {
                ShowMain();
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
            if (sessions::IsWeb(row->tool)) {
                auto copy = *row;
                if (row->tool == Tool::ClaudeWeb)
                    add(L"Open in browser", L"\uE774", [copy] { sessions::OpenInBrowser(copy); });
                auto address = sessions::WebAddress(*row);
                add(L"Copy link", L"\uE71B", [address] { CopyText(address); });
            } else {
                auto path = row->path;
                add(L"Show transcript file", L"\uE838", [path] { sessions::RevealFile(path); });
            }
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

    // ---- Tray and quick search ------------------------------------------

    void ShowMain() {
        if (closing) return;
        HideQuick();
        auto appWindow = window.AppWindow();
        appWindow.Show();
        if (auto presenter = appWindow.Presenter().try_as<OverlappedPresenter>();
            presenter && presenter.State() == OverlappedPresenterState::Minimized)
            presenter.Restore();
        window.Activate();
        HWND handle{};
        window.as<IWindowNative>()->get_WindowHandle(&handle);
        if (handle) SetForegroundWindow(handle);
    }

    static constexpr wchar_t QuickMarkup[] = LR"(
<Grid xmlns='http://schemas.microsoft.com/winfx/2006/xaml/presentation' xmlns:x='http://schemas.microsoft.com/winfx/2006/xaml' x:Name='Backdrop' Background='#01000000'>
  <Grid.RowDefinitions><RowDefinition Height='0.16*'/><RowDefinition Height='Auto'/><RowDefinition Height='*'/></Grid.RowDefinitions>
  <Border Grid.Row='1' x:Name='Panel' Width='780' HorizontalAlignment='Center' CornerRadius='12' Padding='14'
          Background='{ThemeResource SolidBackgroundFillColorBaseBrush}' BorderBrush='{ThemeResource SurfaceStrokeColorDefaultBrush}' BorderThickness='1'>
    <StackPanel Spacing='10'>
      <TextBox x:Name='QuickBox' FontSize='22' PlaceholderText='Find a conversation' AutomationProperties.Name='Find a conversation' IsSpellCheckEnabled='False'/>
      <ListView x:Name='QuickList' SelectionMode='Single' IsItemClickEnabled='True' MaxHeight='620'>
        <ListView.ItemContainerStyle>
          <Style TargetType='ListViewItem'>
            <Setter Property='HorizontalContentAlignment' Value='Stretch'/>
            <Setter Property='Padding' Value='10,6,10,6'/>
            <Setter Property='MinHeight' Value='0'/>
          </Style>
        </ListView.ItemContainerStyle>
      </ListView>
      <TextBlock x:Name='QuickHint' FontSize='12' Foreground='{ThemeResource TextFillColorSecondaryBrush}'
                 Text='&#x2191; &#x2193; to choose  &#xB7;  Enter to open  &#xB7;  Esc to close'/>
    </StackPanel>
  </Border>
</Grid>)";

    void CreateQuick() {
        quick = Window();
        quick.Title(L"Sessions search");
        quick.SystemBackdrop(DesktopAcrylicBackdrop());
        quickRoot = Xaml(QuickMarkup);
        quick.Content(quickRoot);
        quickBox = Named<TextBox>(quickRoot, L"QuickBox");
        quickList = Named<ListView>(quickRoot, L"QuickList");
        quickHint = Named<TextBlock>(quickRoot, L"QuickHint");
        quickItems = single_threaded_observable_vector<IInspectable>();
        quickList.ItemsSource(quickItems);

        // Clicking anywhere outside the panel closes it.
        quickRoot.PointerPressed([this](auto&&, Input::PointerRoutedEventArgs const& e) {
            if (e.OriginalSource() == quickRoot) HideQuick();
        });
        quickList.ItemClick([this](auto&&, ItemClickEventArgs const& e) {
            if (auto row = e.ClickedItem().try_as<FrameworkElement>()) OpenFromQuick(unbox_value<int64_t>(row.Tag()));
        });
        quickBox.PreviewKeyDown([this](auto&&, Input::KeyRoutedEventArgs const& e) {
            using winrt::Windows::System::VirtualKey;
            const int count = int(quickItems.Size());
            int selected = quickList.SelectedIndex();
            switch (e.Key()) {
            case VirtualKey::Down:
                if (count) quickList.SelectedIndex(std::min(count - 1, selected + 1));
                break;
            case VirtualKey::Up:
                if (count) quickList.SelectedIndex(std::max(0, selected - 1));
                break;
            case VirtualKey::PageDown:
                if (count) quickList.SelectedIndex(std::min(count - 1, selected + 8));
                break;
            case VirtualKey::PageUp:
                if (count) quickList.SelectedIndex(std::max(0, selected - 8));
                break;
            case VirtualKey::Enter:
                if (count) {
                    auto item = quickItems.GetAt(uint32_t(std::max(0, selected))).as<FrameworkElement>();
                    OpenFromQuick(unbox_value<int64_t>(item.Tag()));
                }
                break;
            case VirtualKey::Escape:
                HideQuick();
                break;
            default:
                return;
            }
            if (quickList.SelectedItem()) quickList.ScrollIntoView(quickList.SelectedItem());
            e.Handled(true);
        });
        quickDelay = quick.DispatcherQueue().CreateTimer();
        quickDelay.Interval(std::chrono::milliseconds(120));
        quickDelay.IsRepeating(false);
        quickDelay.Tick([this](auto&&, auto&&) { QuickSearch(); });
        quickBox.TextChanged([this](auto&&, auto&&) {
            quickDelay.Stop();
            quickDelay.Start();
        });
        // Leaving the overlay (clicking another window) closes it.
        quick.Activated([this](auto&&, WindowActivatedEventArgs const& e) {
            // (Ignored right after opening, while Windows settles which window is in front.)
            if (e.WindowActivationState() != WindowActivationState::Deactivated && quickBox) quickBox.Focus(FocusState::Programmatic);
            if (e.WindowActivationState() == WindowActivationState::Deactivated && !quickOpening &&
                sessions::NowMs() - quickShownAt > 800)
                HideQuick();
        });
        quick.Closed([this](auto&&, WindowEventArgs const& e) {
            // Closing (Alt+F4) only hides it; Sessions keeps running in the tray.
            if (!closing) {
                e.Handled(true);
                HideQuick();
            }
        });

        auto queue = quick.DispatcherQueue();
        auto weak = get_weak();
        quickSearcher = std::make_unique<Searcher>(databaseFile, [queue, weak](uint64_t ticket, std::wstring, std::vector<Found> results) {
            queue.TryEnqueue([weak, ticket, results = std::move(results)]() mutable {
                if (auto app = weak.get(); app && !app->closing) app->QuickResults(ticket, std::move(results));
            });
        });
    }

    void ShowQuick() {
        if (closing) return;
        if (!quick) CreateQuick();
        // A borderless, always-on-top window covering the whole monitor the
        // pointer is on (taskbar included).
        auto appWindow = quick.AppWindow();
        auto presenter = OverlappedPresenter::Create();
        presenter.SetBorderAndTitleBar(false, false);
        presenter.IsResizable(false);
        presenter.IsMaximizable(false);
        presenter.IsMinimizable(false);
        presenter.IsAlwaysOnTop(true);
        appWindow.SetPresenter(presenter);
        POINT cursor{};
        GetCursorPos(&cursor);
        auto display = DisplayArea::GetFromPoint({cursor.x, cursor.y}, DisplayAreaFallback::Primary);
        appWindow.MoveAndResize(display.OuterBounds());
        appWindow.IsShownInSwitchers(false);
        quickShownAt = sessions::NowMs();
        appWindow.Show();
        quick.Activate();
        HWND handle{};
        quick.as<IWindowNative>()->get_WindowHandle(&handle);
        if (handle) SetForegroundWindow(handle);
        quickBox.Text(L"");
        QuickSearch();
        quickBox.Focus(FocusState::Programmatic);
        // Focus again once the window has finished coming forward.
        quick.DispatcherQueue().TryEnqueue(Microsoft::UI::Dispatching::DispatcherQueuePriority::Low, [this] {
            if (quickBox) quickBox.Focus(FocusState::Keyboard);
        });
    }

    void HideQuick() {
        if (!quick) return;
        if (quickDelay) quickDelay.Stop();
        quick.AppWindow().Hide();
    }

    void OpenFromQuick(int64_t id) {
        quickOpening = true;
        HideQuick();
        quickOpening = false;
        Open(id);
    }

    void QuickSearch() {
        std::wstring query = quickBox.Text().c_str();
        while (!query.empty() && iswspace(query.back())) query.pop_back();
        while (!query.empty() && iswspace(query.front())) query.erase(query.begin());
        if (query.empty()) {
            // Most recent conversations first.
            ++quickTicket;
            std::vector<Found> recent;
            for (size_t i = 0; i < sessionRows.size() && i < 12; ++i) recent.push_back({sessionRows[i].id, 0, {}});
            ShowQuickRows(recent);
            return;
        }
        quickTicket = quickSearcher->Ask(query);
    }

    void QuickResults(uint64_t ticket, std::vector<Found> results) {
        if (ticket != quickTicket) return;
        if (results.size() > 40) results.resize(40);
        ShowQuickRows(results);
    }

    void ShowQuickRows(std::vector<Found> const& results) {
        std::vector<IInspectable> rows;
        for (auto const& result : results)
            if (auto row = Row(result.session)) rows.push_back(QuickRow(*row, result.byMeaning ? L"\x03" + result.snippet : result.snippet));
        quickItems.ReplaceAll(rows);
        if (!rows.empty()) {
            quickList.SelectedIndex(0);
            quickList.ScrollIntoView(quickItems.GetAt(0));
        }
        quickHint.Text(rows.empty() ? L"No conversation matches. Try other words."
                                    : L"\u2191 \u2193 to choose  \u00B7  Enter to open  \u00B7  Esc to close");
    }

    // One line per conversation: logo, title, folder and date, and the matching passage.
    FrameworkElement QuickRow(sessions::SessionRow const& row, std::wstring const& snippet) {
        Grid grid;
        grid.Tag(box_value(row.id));
        grid.ColumnSpacing(12);
        ColumnDefinition badgeColumn, textColumn, dateColumn;
        badgeColumn.Width(GridLengthHelper::FromPixels(32));
        textColumn.Width(GridLengthHelper::FromValueAndType(1, GridUnitType::Star));
        dateColumn.Width(GridLengthHelper::Auto());
        grid.ColumnDefinitions().Append(badgeColumn);
        grid.ColumnDefinitions().Append(textColumn);
        grid.ColumnDefinitions().Append(dateColumn);
        auto badge = Badge(row.tool);
        badge.VerticalAlignment(VerticalAlignment::Center);
        grid.Children().Append(badge);

        StackPanel text;
        text.Spacing(1);
        Grid::SetColumn(text, 1);
        TextBlock title;
        title.Text(row.title);
        title.FontSize(15);
        title.FontWeight(Microsoft::UI::Text::FontWeights::SemiBold());
        title.TextTrimming(TextTrimming::CharacterEllipsis);
        title.MaxLines(1);
        text.Children().Append(title);
        if (!snippet.empty()) {
            auto passage = SnippetBlock(snippet);
            passage.MaxLines(1);
            text.Children().Append(passage);
        } else if (!row.folder.empty()) {
            TextBlock folder;
            folder.Text(row.folder);
            folder.FontSize(12);
            folder.Foreground(ThemeBrush(L"TextFillColorSecondaryBrush"));
            text.Children().Append(folder);
        }
        grid.Children().Append(text);

        TextBlock when;
        when.Text((row.folder.empty() || snippet.empty() ? L"" : row.folder + L"  \u00B7  ") + FriendlyDate(row.lastActivity));
        when.FontSize(12);
        when.Foreground(ThemeBrush(L"TextFillColorSecondaryBrush"));
        when.VerticalAlignment(VerticalAlignment::Center);
        Grid::SetColumn(when, 2);
        grid.Children().Append(when);
        Automation::AutomationProperties::SetName(grid, hstring(row.title));
        return grid;
    }

    // ---- Status and settings ---------------------------------------------

    sessions::IndexStatus lastIndexStatus;

    void ShowStatus(sessions::IndexStatus const& status) {
        lastIndexStatus = status;
        using State = sessions::IndexStatus::State;
        const bool working = status.state == State::Scanning || status.state == State::Indexing;
        busy.IsActive(working);
        busy.Visibility(working ? Visibility::Visible : Visibility::Collapsed);
        doneIcon.Visibility(status.state == State::Ready ? Visibility::Visible : Visibility::Collapsed);
        problemIcon.Visibility(status.state == State::Failed ? Visibility::Visible : Visibility::Collapsed);
        indexMessage = status.message;
        ShowCombinedStatus();
        if (status.dataChanged) {
            Reload();
            if (!activeQuery.empty() && status.state == State::Ready) {
                activeQuery.clear();  // refresh results against the new data
                StartSearch();
            }
        }
    }

    // One row per site: logo, who is signed in (or what to do), live progress,
    // and the one button that makes sense next.
    std::wstring webNotice;  // a one-off message, for example after signing out
    void ShowWebInfo() {
        if (!webSites || !webSync) return;
        webSites.Children().Clear();
        std::vector<std::pair<sessions::WebSync*, sessions::WebSync::SiteStatus>> rows;
        for (auto const& site : webSync->Statuses()) {
            rows.emplace_back(webSync.get(), site);
            if (site.tool == Tool::ChatGptWeb && webSync2)
                for (auto const& second : webSync2->Statuses()) rows.emplace_back(webSync2.get(), second);
        }
        for (auto const& [sync, site] : rows) {
            std::wstring name = site.tool == Tool::ClaudeWeb ? L"Claude" : sessions::ToolName(site.tool);
            if (sync->Slot() > 1) name += L" \u00B7 second account";
            std::wstring now = sync->Live(site.tool);
            // Signed in if the check running now says so, otherwise per the last finished check.
            const bool signedIn = sync->SignedInNow(site.tool) || (now.empty() || now.rfind(L"Waiting", 0) == 0 || now.rfind(L"Next", 0) == 0
                                                                          ? site.signedIn
                                                                          : false) ||
                                  (!now.empty() && site.signedIn);
            const bool unreachable = site.problem.find(L"could not reach") != std::wstring::npos ||
                                     site.problem.find(L"stopped answering") != std::wstring::npos;
            std::wstring text;
            bool needsYou = false;
            if (!now.empty()) {
                text = now + L".";
                if (signedIn && now.rfind(L"Signed in", 0) != 0) text = L"Signed in. " + text;
            } else if (!site.checked) {
                text = sync->Slot() > 1 ? L"Optional: sign in here with another ChatGPT account, such as a work one."
                                        : L"Not signed in yet. Press Sign in.";
                needsYou = sync->Slot() == 1;
            } else if (site.signedIn) {
                text = L"Signed in as " + (site.who.empty() ? std::wstring(L"your account") : site.who) + L". Nothing to do.";
            } else if (unreachable) {
                text = L"The site didn't answer at the last check. Nothing to do; Sessions tries again by itself.";
            } else {
                text = L"Not signed in. Press Sign in.";
                needsYou = true;
            }
            if (now.empty() && site.chats) text += L" " + Number(site.chats) + (site.chats == 1 ? L" chat in Sessions." : L" chats in Sessions.");
            if (now.empty() && site.checkedAt > 0) text += L" Last checked " + FriendlyDate(site.checkedAt) + L".";

            Border card;
            card.CornerRadius({8, 8, 8, 8});
            card.Padding({12, 10, 12, 10});
            card.Background(ThemeBrush(L"CardBackgroundFillColorDefaultBrush"));
            Grid row;
            row.ColumnSpacing(10);
            ColumnDefinition logoColumn, textColumn, buttonColumn;
            logoColumn.Width(GridLengthHelper::FromPixels(32));
            textColumn.Width(GridLengthHelper::FromValueAndType(1, GridUnitType::Star));
            buttonColumn.Width(GridLengthHelper::Auto());
            row.ColumnDefinitions().Append(logoColumn);
            row.ColumnDefinitions().Append(textColumn);
            row.ColumnDefinitions().Append(buttonColumn);
            auto logo = Badge(site.tool);
            row.Children().Append(logo);
            StackPanel words;
            Grid::SetColumn(words, 1);
            TextBlock title;
            title.Text(name);
            title.FontWeight(Microsoft::UI::Text::FontWeights::SemiBold());
            words.Children().Append(title);
            TextBlock state;
            state.Text(text);
            state.FontSize(12);
            state.TextWrapping(TextWrapping::Wrap);
            state.Foreground(ThemeBrush(needsYou ? L"SystemFillColorCautionBrush" : L"TextFillColorSecondaryBrush"));
            // The saved problem belongs to the last check; don't show it beside a row that says signed in.
            if (!site.problem.empty() && !signedIn) ToolTipService::SetToolTip(state, box_value(site.problem));
            words.Children().Append(state);
            row.Children().Append(words);

            StackPanel actions;
            actions.Spacing(2);
            actions.VerticalAlignment(VerticalAlignment::Center);
            Grid::SetColumn(actions, 2);
            Button signIn;
            // The button matches the state: "Sign in" (highlighted) only when something
            // is needed; otherwise "Switch account" for using a different account.
            signIn.Content(box_value(signedIn ? L"Switch account" : L"Sign in"));
            if (needsYou) signIn.Style(Application::Current().Resources().Lookup(box_value(L"AccentButtonStyle")).as<Style>());
            auto tool = site.tool;
            signIn.Click([this, sync, tool, signedIn](auto&&, auto&&) {
                if (signedIn) {
                    // Switching account: sign out first so the site shows its account chooser.
                    sync->SignOut(tool, [this, sync, tool](std::wstring) {
                        if (!closing) sync->SignIn(tool);
                    });
                } else {
                    sync->SignIn(tool);
                }
            });
            actions.Children().Append(signIn);
            if (signedIn) {
                HyperlinkButton out;
                out.Content(box_value(L"Sign out"));
                out.Padding({4, 0, 4, 0});
                out.FontSize(12);
                out.Click([this, sync, tool](auto&&, auto&&) {
                    sync->SignOut(tool, [this](std::wstring message) {
                        if (closing) return;
                        webNotice = message;
                        ShowWebInfo();
                    });
                });
                actions.Children().Append(out);
            }
            row.Children().Append(actions);
            card.Child(row);
            webSites.Children().Append(card);
        }
        if (!webNotice.empty()) {
            TextBlock notice;
            notice.Text(webNotice);
            notice.FontSize(12);
            notice.TextWrapping(TextWrapping::Wrap);
            notice.Foreground(ThemeBrush(L"TextFillColorSecondaryBrush"));
            webSites.Children().Append(notice);
            webNotice.clear();
        }
    }

    void ShowCombinedStatus() {
        std::wstring web = webMessage.empty() ? webMessage2 : webMessage;
        std::wstring text = web.empty() ? indexMessage : L"Syncing web chats \u00B7 " + web;
        statusText.Text(text);
        tray.SetTip((L"Sessions \u00B7 " + text).c_str());
        ToolTipService::SetToolTip(statusText, box_value(text));
        using State = sessions::IndexStatus::State;
        const bool working = !web.empty() || lastIndexStatus.state == State::Scanning || lastIndexStatus.state == State::Indexing;
        busy.IsActive(working);
        busy.Visibility(working ? Visibility::Visible : Visibility::Collapsed);
        doneIcon.Visibility(!working && lastIndexStatus.state == State::Ready ? Visibility::Visible : Visibility::Collapsed);
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
        ShowWebInfo();
        accountsPanel.Children().Clear();
        for (auto const& account : accountRows) {
            Border card;
            card.CornerRadius({8, 8, 8, 8});
            card.Padding({12, 10, 12, 12});
            card.Background(ThemeBrush(L"CardBackgroundFillColorDefaultBrush"));
            StackPanel stack;
            stack.Spacing(6);
            TextBlock heading;
            heading.Text((account.id.rfind("gemini:", 0) == 0          ? L"Gemini  ·  "
                          : account.id.rfind("antigravity:", 0) == 0 ? L"Antigravity  ·  "
                          : account.family == sessions::Family::Codex ? L"ChatGPT / Codex  ·  "
                                                                       : L"Claude  ·  ") +
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
                           L" helper or empty transcripts are left out.");
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
    bool scan = false, keys = false;  // --keys: every result's session key, in order (for scoring tests)
    for (int i = 1; i < argc; ++i) {
        std::wstring_view arg(argv[i]);
        if (arg == L"--index") scan = true;
        else if (arg == L"--db" && i + 1 < argc) database = argv[++i];
        else if (arg == L"--search" && i + 1 < argc) queries.push_back(argv[++i]);
        else if (arg == L"--keys") keys = true;
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
            if (keys) {
                std::wstring line = L"keys";
                for (auto const& hit : hits)
                    if (auto row = byId[hit.session]) line += L"	" + sessions::Wide(row->key);
                print(line);
            }
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

namespace {

int RunWebSyncHeadless(int argc, wchar_t** argv) {
    std::filesystem::path database = sessions::Index::DefaultFile(), profile;
    for (int i = 1; i < argc; ++i) {
        std::wstring_view arg(argv[i]);
        if (arg == L"--db" && i + 1 < argc) database = argv[++i];
        else if (arg == L"--profile" && i + 1 < argc) profile = argv[++i];
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
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    bool finished = false;
    sessions::Index index(database);
    std::unique_ptr<sessions::WebSync> sync;
    sync = std::make_unique<sessions::WebSync>(index, [&](std::wstring const& status, bool, bool done) {
        if (!status.empty()) print(L"  " + status);
        if (done) {
            print(WebSummary(*sync, true));
            finished = true;
            PostQuitMessage(0);
        }
    }, profile);
    sync->SyncAll();
    MSG msg{};
    while (!finished && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    sync.reset();
    return 0;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    int argc{};
    auto argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 1; argv && i < argc; ++i) {
        std::wstring_view arg(argv[i]);
        if (arg == L"--websync") {
            int code = RunWebSyncHeadless(argc, argv);
            LocalFree(argv);
            return code;
        }
        if (arg == L"--import-chatgpt" && i + 1 < argc) {
            std::filesystem::path database = sessions::Index::DefaultFile();
            for (int j = 1; j + 1 < argc; ++j)
                if (std::wstring_view(argv[j]) == L"--db") database = argv[j + 1];
            AttachConsole(ATTACH_PARENT_PROCESS);
            auto print = [](std::wstring const& text) {
                auto utf8 = sessions::Utf8(text + L"\n");
                DWORD written;
                WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), utf8.data(), DWORD(utf8.size()), &written, nullptr);
            };
            auto result = sessions::ImportChatGptExport(database, argv[i + 1], print);
            print(L"imported " + std::to_wstring(result.conversations) + L", skipped " + std::to_wstring(result.skipped) +
                  (result.problem.empty() ? L"" : L" \u00B7 " + result.problem));
            LocalFree(argv);
            return result.conversations || result.problem.empty() ? 0 : 1;
        }
        if (arg == L"--index" || arg == L"--search") {
            int code = RunHeadless(argc, argv);
            LocalFree(argv);
            return code;
        }
    }
    if (argv) LocalFree(argv);

    // One copy at a time: two copies would share (and could damage) the web
    // sign-in browser profile and the database. A second launch shows the
    // running copy instead.
    HANDLE single = CreateMutexW(nullptr, TRUE, L"Local\\ShaunobiSessionsApp");
    if (single && GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND tray = FindWindowW(L"SessionsTrayWindow", nullptr);
        if (tray) {
            DWORD process{};
            GetWindowThreadProcessId(tray, &process);
            AllowSetForegroundWindow(process);
            PostMessageW(tray, WM_APP + 41, 1, WM_CONTEXTMENU);  // same as right-clicking the tray icon: open the window
        }
        CloseHandle(single);
        return 0;
    }
    try {
        init_apartment(apartment_type::single_threaded);
        Application::Start([](auto&&) {
            auto app = make_self<App>();
            int count{};
            auto args = CommandLineToArgvW(GetCommandLineW(), &count);
            for (int i = 1; args && i < count; ++i)
                if (std::wstring_view(args[i]) == L"--tray") app->startInTray = true;
                else if (std::wstring_view(args[i]) == L"--quick") app->startInTray = app->startQuick = true;
            if (args) LocalFree(args);
        });
    } catch (hresult_error const& e) {
        MessageBoxW(nullptr, e.message().c_str(), L"Sessions could not start", MB_OK | MB_ICONERROR);
        return 1;
    }
    return 0;
}
