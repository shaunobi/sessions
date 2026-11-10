# Sessions

A Windows 11 app for finding AI coding sessions: "I had a session about X; was it Codex or Claude, and which one?" Every session is a small card in a grid. Search what you or the AI said, filter by tool, account or tag, and click a card to open the session in its own app. Native C++20 and WinUI 3, following CardFX's structure. Planning decisions are in [docs/decisions.md](docs/decisions.md).

## Build, run, install

Install Visual Studio 2022 with **Desktop development with C++** and Windows SDK **10.0.26100.0**.

```powershell
.\build.ps1      # restores packages and builds Release
.\run.ps1        # runs the build
.\install.ps1    # copies it to D:\Apps\Sessions and adds Start menu and desktop shortcuts
```

C: is nearly full, so packages and build output go to **`D:\Build\Sessions`** (override with `-BuildRoot` or `SESSIONS_BUILD_ROOT`). The installed folder is **`D:\Apps\Sessions`**: the executable, the Windows App SDK runtime files beside it, and one database, `Sessions.db`. Installing again keeps the database.

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

- Model: **BGE small English v1.5** (BAAI, MIT licence, 384 dimensions, 133 MB), downloaded by `build.ps1` from Hugging Face at a pinned revision into `D:\Build\Sessions\models` and installed to `models\` beside the executable. It is not stored in the repository. Without it, meaning search is simply off.
- Runtime: ONNX Runtime from **Windows Machine Learning** (`Microsoft.Windows.AI.MachineLearning`), on the GPU through DirectML, falling back to the CPU. The tokenizer (BERT WordPiece, uncased) is written in-house in `src/Tokenizer.cpp`.
- Messages are split into pieces of up to 254 tokens (at most eight per message). Each piece's vector is stored once in the database, keyed by a hash of its tokens, as 384 signed bytes; re-reading a live session only embeds the new pieces. Vectors are searched in memory by brute force, which takes a few milliseconds at this size, so the SQLite vector add-on was not needed.
- On this PC (RTX 3070) the first pass over all ~20,500 messages takes about 80 seconds after keyword indexing, and the status shows "Learning meaning" with progress. A search, including embedding the query, takes 15–200 ms. The database grows from about 20 MB to about 33 MB.

## Opening sessions

- **Claude desktop**: `claude://code/continue?session=<local id>`. The app recognizes the link; whether it jumps straight to the session is still to be confirmed. At worst Claude comes to the front.
- **Codex desktop**: `codex://threads/<thread id>`.
- **Claude CLI / Codex CLI**: a Windows Terminal tab in the session's folder running `claude --resume <id>` or `codex resume <id>`.

Right-click a card to add or remove tags, copy the session id, or show the transcript file in Explorer.

## Tests

```powershell
.\tests\Test-Index.ps1           # fresh index into a scratch database; checks all four tools and search
.\tests\Try-Search.ps1 -Query 'zoom' -OutputPath D:\Build\Sessions\shot.png   # types into the open window and captures it
```

`Sessions.exe --index [--db file] [--search "words"]` runs one scan (and the meaning pass) without a window and prints a summary, with meaning-only results marked `[meaning NN]`. It is what the test uses.

## Libraries

Compiled in, with licences under `third_party`: **SQLite 3.54.1** (public domain, FTS5 enabled) and the **Zstandard 1.5.7** decompressor only (BSD). Windows App SDK 2.5.1, Windows Machine Learning 2.1.74 (ONNX Runtime and DirectML) and C++/WinRT come from NuGet as in CardFX. The embedding model is a separate download (above).

## Not done yet

- **Copying sessions between tools and accounts** (the research track in the decisions log).
- **Antigravity** desktop and command line.
- **The icon**: `assets/Sessions.ico` is an interim drawing from `tools/MakePlaceholderIcon.ps1`; the Blender render will replace it.
