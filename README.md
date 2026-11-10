# Sessions

A Windows 11 app for finding AI coding sessions: "I had a session about X; was it Codex or Claude, and which one?" Every session is a small card in a grid. Search what you or the AI said, filter by tool, account or tag, and click a card to open the session in its own app. Native C++20 and WinUI 3, following CardFX's structure. Planning decisions are in [docs/decisions.md](docs/decisions.md).

## Build, run, install

Install Visual Studio 2022 with **Desktop development with C++** and Windows SDK **10.0.26100.0**.

```powershell
.\build.ps1      # restores packages and builds Release
.\run.ps1        # runs the build
.\install.ps1    # copies it to D:\Apps\Sessions and adds Start menu and desktop shortcuts
```

C: is nearly full, so packages and build output go to **`D:\Build\Sessions`** (override with `-BuildRoot` or `SESSIONS_BUILD_ROOT`). The installed folder **`D:\Apps\Sessions`** holds three files:

- `Sessions.exe` (about 135 MB). The embedding model, its vocabulary, the app image and the tool logos are compiled into it as resources.
- `Microsoft.WindowsAppRuntime.Bootstrap.dll`, the small loader that finds the Windows App Runtime. WinUI and Windows Machine Learning (ONNX Runtime, DirectML) come from that runtime, which is installed in Windows (2.5 or later), rather than being copied beside the app. Unlike CardFX, the build is framework-dependent, so there are no runtime DLLs in the folder.
- `Sessions.db`, the index. While the app is open, SQLite keeps `Sessions.db-wal` and `Sessions.db-shm` beside it, and they go away when it closes.
- `Browser\`, the profile of the built-in browser used for web chat sign-ins (created when web sync first runs).

Installing again mirrors the build into the folder: leftovers from older builds are removed and the database is kept.

## Sources

| Tool | Where it is read from | How it is told apart | Account |
|---|---|---|---|
| Claude desktop | `~\.claude\projects\*\*.jsonl` | `entrypoint: claude-desktop`, or a sidebar entry under `%APPDATA%\Claude\claude-code-sessions\<account>\<organization>\local_*.json` | the organization folder of that sidebar entry |
| Claude CLI | same transcripts | any other entrypoint | `bridge-session` owner organization in the transcript; otherwise the organization the command line is signed in to now (`oauthAccount` in `~\.claude.json`) |
| Codex desktop | `~\.codex\sessions\**\rollout-*.jsonl[.zst]` and `archived_sessions` | `session_meta.source = vscode` or originator `Codex Desktop` | `creator_account_id` |
| Codex CLI | same rollouts | `cli` and `exec` sources | `creator_account_id` (older rollouts have none) |

Titles come from the apps' own lists where they exist (the Claude desktop sidebar entry, Codex's `state_5.sqlite` and `session_index.jsonl`, both opened read-only), then from titles written into the transcript, then from the first prompt. Claude subagent transcripts and Codex subagent and review-helper threads are skipped. Antigravity is not read yet.

Only what the person and the AI said is indexed: prompts, the AI's text replies, and in Claude project threads the replies sent through `post_message`. Tool calls, command output, thinking and injected context (system reminders, environment blocks, command echoes) are left out. Messages keep a `kind` column so tool activity can be added later. Nothing is ever written to another tool's files.

Account names are not stored on disk by either tool, so they are set in **Settings** (the gear in the upper right). Rebuild there re-reads one account's sessions; Rebuild everything starts over.

## How it stays current

Nothing runs while the window is closed. On launch the app compares every transcript's size and modification time with what the database last saw and re-reads only what changed (newest first, on background-priority threads). While open it watches the four source folders and catches up a few seconds after changes settle, at most every 15 seconds, since live sessions write constantly. The status in the upper right shows scanning, indexing progress, or a green check.

On this PC a full index from empty takes about 4 seconds: 3,681 files, of which 409 are conversations (about 20,000 messages); the rest are helper threads. The database is about 20 MB. Searches take a few milliseconds; very common words take tens.

## Search

Each search runs two ways at once and merges the results:

- **Words.** Every word must appear in the session, in its messages or its title. Common words ("the", "to", "that") are ignored unless quoted, the last word also matches as a prefix while typing, and `"quoted phrases"` match exactly. Sessions where one message holds all the words rank ahead of sessions where they are scattered. The card shows the best passage with the words highlighted. This uses SQLite FTS5.
- **Meaning.** The query and every message are turned into meaning vectors by a small local model, so "trouble signing in to the cloud" finds a Cloudflare sign-in problem that never used those words. Cards found this way are marked **≈ similar meaning** and show the closest passage.

The two rankings are merged by reciprocal rank fusion. Short queries (one or two words) lean on the words; sentences lean on meaning. While a search is active the sort switches to **Best match**, and it goes back to **Last activity** when the search is cleared, unless you picked a sort yourself.

### Meaning search details

- Model: **BGE small English v1.5** (BAAI, MIT licence, 384 dimensions, 133 MB). `build.ps1` downloads it from Hugging Face at a pinned revision into `D:\Build\Sessions\models`, and it is compiled into the executable. It is not stored in the repository.
- Runtime: ONNX Runtime from **Windows Machine Learning** (`Microsoft.Windows.AI.MachineLearning`), on the GPU through DirectML, falling back to the CPU. The tokenizer (BERT WordPiece, uncased) is written in-house in `src/Tokenizer.cpp`.
- Messages are split into pieces of up to 254 tokens (at most eight per message). Each piece's vector is stored once in the database, keyed by a hash of its tokens, as 384 signed bytes; re-reading a live session only embeds the new pieces. Vectors are searched in memory by brute force, which takes a few milliseconds at this size, so the SQLite vector add-on was not needed.
- On this PC (RTX 3070) the first pass over all ~20,500 messages takes about 80 seconds after keyword indexing, and the status shows "Learning meaning" with progress. A search, including embedding the query, takes 15–200 ms. The database grows from about 20 MB to about 33 MB.

## Web chats: Claude, ChatGPT, Gemini

Besides the coding sessions on disk, Sessions syncs your conversations from **claude.ai**, **chatgpt.com** and **gemini.google.com**, using each site's own backend: the same requests the website makes.

- **Sign in once** in Settings › Web chats (Claude, ChatGPT, Gemini). That opens Sessions' own browser at the site's sign-in page. You sign in on the site itself, and Sessions never sees your password. A sync starts as soon as the site shows a page other than its sign-in pages, and again when you close the window. The sign-ins are kept in the `Browser` folder beside the executable.
- **Syncing** runs 20 seconds after launch and then every 30 minutes while Sessions is open, or on **Sync now**. It reads only conversations that are new or changed since the last sync, and removes ones the site no longer lists. The status line shows progress, and Settings shows each site's last result and any problem.
- **Coverage**: Claude reads every organization of the signed-in account (so both Personal and Sembit). ChatGPT reads the personal account and any workspaces, including archived chats and chats inside projects; the project name shows as the folder. Gemini reads the account's chat list.
- Web conversations are searched like everything else, carry the service's logo with a globe badge, and open in your default browser (`claude.ai/chat/…`, `chatgpt.com/c/…`, `gemini.google.com/app/…`). The tool filter has a choice for each.
- **These backends are not public interfaces.** When a site changes, its sync reports an error in Settings; the scripts to fix are in `src/WebAgents.h`.

## Window

Sessions opens where it was last closed or minimized, at the same size, maximized if it was. The first time, or if that spot is no longer on a screen, it opens centered on the screen under the pointer, wide enough for four columns of cards.

## Tray and quick search

Sessions lives in the notification area (lower right of the taskbar) while it runs.

- **Click the icon** for quick search: a frosted full-screen overlay with one search box at the top. It lists the most recent conversations straight away and narrows them as you type (words and meaning, as in the main window). Use **↑ ↓** (or Page Up/Down) to choose and **Enter** to open the conversation in its own app; the overlay disappears. **Esc**, clicking outside the panel or switching to another window closes it. There is deliberately no keyboard shortcut.
- **Right-click the icon** to open the full app.
- **Minimizing** the full app hides it to the tray instead of the taskbar. **Closing** it quits Sessions, tray icon included.
- The icon's tooltip shows the same status as the window: indexing progress, or "Up to date · N sessions · N messages".
- `Sessions.exe --tray` starts with only the tray icon, which suits starting at sign-in. `--quick` starts straight into quick search.

## Cards

Each card shows the tool's own logo, the session's name as its app shows it, its folder, when it was last active and how many messages it has. Command-line sessions carry a small `>_` terminal chip on the logo; Claude Code's command line has its own dark logo as well. The account appears in the logo's tooltip and in the Account filter.

- **Names**: the Claude desktop sidebar title; for Codex, the sidebar name from `~\.codex\session_index.jsonl` (generated or renamed), then the thread's name in `state_5.sqlite`. Codex's `threads.title` is usually just the first prompt, so it is used only when nothing else exists.
- **Folders**: for Codex, the project the thread belongs to (`threads.project_id`), otherwise the first project in sidebar order whose folder contains the session's folder (`projects` and `project_roots` in `state_5.sqlite`). For example, `D:\ownCloud\Management` shows as **sembit**. Otherwise the last part of the session's folder; Claude desktop sessions use the folder they were started in (`originCwd`). Scratch sessions show no folder.
- **Logos** belong to their companies, so they are not committed. `tools\ExtractLogos.ps1` copies them at the highest resolution installed (ChatGPT's white knot from the Codex app, the Claude app tile up to 1240 px, Claude Code's own icon, Antigravity's icon) into `assets\logos`, and `build.ps1` runs it when they are missing.

## Opening sessions

- **Claude desktop**: `claude://code/continue?session=<local id>`. Claude looks the id up only among the non-archived sessions of the organization it is currently showing; if the session is not there, it quietly opens a new session instead. Sessions therefore guesses which organization Claude is showing (the one whose session was focused most recently) and, for a session in the other organization, asks you to switch organizations in Claude first.
- **Codex desktop**: `codex://threads/<thread id>`.
- **Claude CLI / Codex CLI**: a Windows Terminal tab in the session's folder running `claude --resume <id>` or `codex resume <id>`.

Right-click a card to add or remove tags, copy the session id, or show the transcript file in Explorer.

## Tests

```powershell
.\tests\Test-Index.ps1           # fresh index into a scratch database; checks all four tools and search
.\tests\Try-Search.ps1 -Query 'zoom' -OutputPath D:\Build\Sessions\shot.png   # types into the open window and captures it
```

`Sessions.exe --websync [--db file] [--profile folder]` runs one web sync without a window and prints its progress and each site's result. `Sessions.exe --index [--db file] [--search "words"]` runs one scan (and the meaning pass) without a window and prints a summary, with meaning-only results marked `[meaning NN]`. It is what the test uses.

## Libraries

Compiled in, with licences under `third_party`: **SQLite 3.54.1** (public domain, FTS5 enabled) and the **Zstandard 1.5.7** decompressor only (BSD). Windows App SDK 2.5.1, Windows Machine Learning 2.1.74 (ONNX Runtime and DirectML) and C++/WinRT come from NuGet as in CardFX. The embedding model is a separate download (above).

## Not done yet

- **Copying sessions between tools and accounts** (the research track in the decisions log).
- **Antigravity** desktop and command line.
- **The icon**: `assets/Sessions.ico` is an interim drawing from `tools/MakePlaceholderIcon.ps1`; the Blender render will replace it.
