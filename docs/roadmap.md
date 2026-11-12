# Roadmap

imcode runs coding agents (Claude Code, Codex, Gemini, any ACP agent) in folders on this machine or on machines reached over ssh, several at once, from desktop and phone.

Order is by phase; within a phase, top first. `[x]` done, `[~]` partly. Each open item links its issue; bugs and improvements found along the way are [issues](https://github.com/dunkbing/imcode/issues) too.

## 1. Chat

- [x] Threads: an agent in a folder, several running at once, saved across launches and resumed with `session/load`
- [x] Transcript: messages, thoughts, tool calls, plans; permission and sign-in prompts
- [x] Model, mode and effort pickers from the agent's config options, there before the first message
- [~] Markdown (done, imgui_rich_md) and highlighted code blocks ([#1](https://github.com/dunkbing/imcode/issues/1))
- [x] Fonts and icons: JetBrains Mono, an icon font, the system CJK font
- [x] Slash commands and `@file` mentions in the composer
- [x] Tool call details: diffs for edits, output for commands, files that open in the viewer
- [~] Agents install themselves: managed Bun, here and on a remote (done); registry binaries, with progress ([#2](https://github.com/dunkbing/imcode/issues/2))
- [~] Queue a message while a turn runs (done); edit and resend the last one ([#3](https://github.com/dunkbing/imcode/issues/3))
- [x] Images and files as attachments: picked, dropped on the window or pasted; uploaded to the host for a remote thread
- [~] Context and cost from `usage_update`, and the agent's own title for a thread (done); compaction notices ([#4](https://github.com/dunkbing/imcode/issues/4))
- [x] Rename, archive, search threads
- [~] Detect installed agents: turn on, install, sign in, update; browse the ACP registry ([#5](https://github.com/dunkbing/imcode/issues/5))
- [ ] Add a local ACP command as an agent; several accounts or configs of one agent ([#6](https://github.com/dunkbing/imcode/issues/6))
- [~] Import projects and recent threads from agent CLI history (Claude Code and Codex done, Pi) ([#7](https://github.com/dunkbing/imcode/issues/7))
- [~] Model, effort and mode remembered for new threads (done); custom models per agent ([#8](https://github.com/dunkbing/imcode/issues/8))
- [x] Permission mode per thread, shown by the composer, shift+tab to change; remembered for new threads except modes that allow everything
- [~] Approve or reject requests in the transcript (done); a subagent's requests asked in the parent ([#9](https://github.com/dunkbing/imcode/issues/9))
- [ ] Agent questions: pick an option or type an answer, with files attached ([#10](https://github.com/dunkbing/imcode/issues/10))
- [~] Plan and task lists as a card (done); a proposed plan to accept or revise ([#11](https://github.com/dunkbing/imcode/issues/11))
- [~] Send a queued message now, stopping the running turn (done); steer or queue, per message ([#12](https://github.com/dunkbing/imcode/issues/12))
- [ ] Queued messages held after a stop or restart, with resume ([#13](https://github.com/dunkbing/imcode/issues/13))
- [~] Interrupt a turn (done); force-stop an unresponsive one ([#14](https://github.com/dunkbing/imcode/issues/14))
- [ ] `$skill` mentions; skills in the slash menu; restart the agent session after changing skills or MCP ([#15](https://github.com/dunkbing/imcode/issues/15))
- [ ] `/compact`, `/clear` (a fresh session in the same thread), `/goal` with progress ([#16](https://github.com/dunkbing/imcode/issues/16))
- [ ] `@thread` mentions and thread links as context; drag a thread onto the composer ([#17](https://github.com/dunkbing/imcode/issues/17))
- [ ] `@model` mentions to name a model when delegating; `#` to mention a pull request ([#18](https://github.com/dunkbing/imcode/issues/18))
- [ ] Context chips inline in the message: terminal excerpt, diff comment, file, picked element ([#19](https://github.com/dunkbing/imcode/issues/19))
- [ ] Rich-text markdown composer with code blocks; a plain mode ([#20](https://github.com/dunkbing/imcode/issues/20))
- [x] Large pastes become text-file attachments
- [ ] Quote part of a reply into the composer, with a comment ([#21](https://github.com/dunkbing/imcode/issues/21))
- [~] Recall sent prompts with up and down (done); stash a draft and restore it later ([#22](https://github.com/dunkbing/imcode/issues/22))
- [ ] Suggested next prompt after a turn ([#23](https://github.com/dunkbing/imcode/issues/23))
- [ ] Edit an earlier prompt: rewind the conversation, keep or revert the files ([#24](https://github.com/dunkbing/imcode/issues/24))
- [ ] Fork a thread; switch agent mid-thread with the context handed over ([#25](https://github.com/dunkbing/imcode/issues/25))
- [ ] Side chat: ask about a thread without adding to it, bring the answer back ([#26](https://github.com/dunkbing/imcode/issues/26))
- [ ] Voice dictation in the composer: on this machine or a host, with transcript cleanup ([#27](https://github.com/dunkbing/imcode/issues/27))
- [ ] Send key: enter or mod+enter; send and start the next thread in the background ([#28](https://github.com/dunkbing/imcode/issues/28))
- [~] Finished tool calls in a row folded into one expandable line (done); hide successful or all; a mode with only the last reply ([#29](https://github.com/dunkbing/imcode/issues/29))
- [ ] Live output of a running command ([#30](https://github.com/dunkbing/imcode/issues/30))
- [~] Find in thread, cmd+f (done); a minimap of the timeline ([#31](https://github.com/dunkbing/imcode/issues/31))
- [~] Copy a message as markdown (done); copy a link to a thread ([#32](https://github.com/dunkbing/imcode/issues/32))
- [ ] Math, mermaid diagrams, tables, images and video in replies; image zoom ([#33](https://github.com/dunkbing/imcode/issues/33))
- [ ] Visual replies: an agent's HTML page rendered sandboxed in the thread; MCP apps ([#34](https://github.com/dunkbing/imcode/issues/34))
- [ ] Preview attached files: code, markdown, CSV, PDF, audio, zip ([#35](https://github.com/dunkbing/imcode/issues/35))
- [~] Elapsed time of the running turn (done); goal, errors; a details card with context, usage, servers, pull requests ([#36](https://github.com/dunkbing/imcode/issues/36))
- [ ] Usage-limit stops: resume at reset, snooze until reset ([#37](https://github.com/dunkbing/imcode/issues/37))
- [ ] Compact idle threads automatically (Claude Code) ([#38](https://github.com/dunkbing/imcode/issues/38))
- [ ] Generated thread titles; regenerate; title instructions ([#39](https://github.com/dunkbing/imcode/issues/39))
- [ ] A thread without a project, in a scratch folder; turn it into a project ([#40](https://github.com/dunkbing/imcode/issues/40))
- [ ] Secret requests: an agent asks for a secret in a private card ([#41](https://github.com/dunkbing/imcode/issues/41))

## 2. Remote over ssh

The agent runs on the remote machine in the remote folder; nothing is installed there but the agent.

- [x] Built-in ssh (libssh2), one connection per host: the ssh agent, `~/.ssh/config`, default keys, a password or passphrase; new host keys are remembered, a changed one is refused
- [x] Open folder: pick a host (from `~/.ssh/config` or typed), then browse its folders
- [x] A thread in a remote folder: the agent runs there with the login shell's `PATH`
- [x] Existing sessions of a host and folder (`session/list`) appear as threads: continue what was started in a terminal
- [~] Sign in to an agent in the thread's own terminal, local or remote (done; not tried against a real sign-in) ([#42](https://github.com/dunkbing/imcode/issues/42))
- [~] Reconnect with resume after a drop or sleep (done), the ssh handshake off the ui thread (done); making a worktree still blocks ([#43](https://github.com/dunkbing/imcode/issues/43))
- [~] Install an agent on the remote: npm agents on a managed bun (done); registry binaries ([#44](https://github.com/dunkbing/imcode/issues/44))
- [~] Passwords and passphrases in the system keychain (done, macOS); Windows sockets ([#45](https://github.com/dunkbing/imcode/issues/45))
- [ ] Runs that survive a disconnect: a small host process on the remote (phase 3) that the app installs and attaches to ([#46](https://github.com/dunkbing/imcode/issues/46))
- [ ] Host picker on a new thread; cycle hosts with a key ([#47](https://github.com/dunkbing/imcode/issues/47))
- [ ] Add a project that exists on one host to another host ([#48](https://github.com/dunkbing/imcode/issues/48))
- [~] A status dot for each host, in the host list and on remote threads (done); saved hosts with a name and icon; a connection diagnosis ([#49](https://github.com/dunkbing/imcode/issues/49))
- [ ] A remote local server opened in the browser panel through a forward ([#50](https://github.com/dunkbing/imcode/issues/50))

## 3. Host and sync

One host per machine owns the agents and the threads; every screen is a client of it.

- [x] Agent logic off the frame loop: a host thread polls the agents, also while the window is in the background
- [ ] Commands in, events out, as JSON; in-process on desktop ([#51](https://github.com/dunkbing/imcode/issues/51))
- [ ] Thread entries in SQLite with row versions; a client that reconnects fetches what changed ([#52](https://github.com/dunkbing/imcode/issues/52))
- [ ] `imcode --serve`: the host without a window, on a Linux box or a Mac ([#53](https://github.com/dunkbing/imcode/issues/53))
- [ ] Links: a channel of the built-in ssh to the host's loopback port; WebSocket with a pairing code on a LAN or tailnet ([#54](https://github.com/dunkbing/imcode/issues/54))
- [ ] Several clients on one thread (desktop and phone) ([#55](https://github.com/dunkbing/imcode/issues/55))
- [ ] The host as a background service (launchd, systemd): install, status, restart ([#56](https://github.com/dunkbing/imcode/issues/56))
- [ ] Pair by QR code or link; list and revoke paired clients ([#57](https://github.com/dunkbing/imcode/issues/57))
- [ ] One host reachable by several routes (LAN, tailnet, ssh), the working one picked ([#58](https://github.com/dunkbing/imcode/issues/58))
- [ ] HTTPS on a tailnet ([#59](https://github.com/dunkbing/imcode/issues/59))
- [ ] Linked hosts: threads on one referenced from another ([#60](https://github.com/dunkbing/imcode/issues/60))
- [ ] Settings kept on the host; edit one host or all connected hosts at once ([#61](https://github.com/dunkbing/imcode/issues/61))
- [ ] Update a connected host from a client; a warning when versions differ ([#62](https://github.com/dunkbing/imcode/issues/62))
- [ ] Guest link: someone follows one thread in a browser and proposes messages you send or dismiss ([#63](https://github.com/dunkbing/imcode/issues/63))
- [ ] Prompt links: a URL that opens a draft with a prompt, repo and model, never sent by itself ([#64](https://github.com/dunkbing/imcode/issues/64))
- [ ] The host as an MCP server: outside agents read, start and message threads, with a scoped sign-in ([#65](https://github.com/dunkbing/imcode/issues/65))
- [~] `imcode <folder>` opens a project from a terminal (done); thread ids for scripts ([#66](https://github.com/dunkbing/imcode/issues/66))
- [ ] Storage page: database and attachment size, reclaim space ([#67](https://github.com/dunkbing/imcode/issues/67))

## 4. Workspace

- [~] Projects: threads grouped by host and folder (done), with the git branch ([#68](https://github.com/dunkbing/imcode/issues/68))
- [~] A git worktree per thread on its own branch, local or over ssh, removed with the thread when clean (done); a setup command ([#69](https://github.com/dunkbing/imcode/issues/69))
- [~] Diff view: the working tree against the last commit, file by file (done); what a turn changed; against the branch base ([#70](https://github.com/dunkbing/imcode/issues/70))
- [ ] Checkpoints as hidden git refs; revert the files of a turn ([#71](https://github.com/dunkbing/imcode/issues/71))
- [x] Commit, push, open a pull request (from the Changes dialog; the pull request through gh)
- [~] Terminal in the thread's folder, local or remote (libghostty); no selection or copy yet ([#72](https://github.com/dunkbing/imcode/issues/72))
- [~] File viewer: read-only, line numbers, simple syntax colours; no selection or search in the file yet ([#73](https://github.com/dunkbing/imcode/issues/73))
- [~] Add project: a local folder, a new git repo, a clone from a URL or a forge; the clone runs in the background ([#74](https://github.com/dunkbing/imcode/issues/74))
- [ ] Publish a local repo to a forge ([#75](https://github.com/dunkbing/imcode/issues/75))
- [ ] Forge accounts: GitHub, GitLab, Forgejo/Gitea, Bitbucket, Azure DevOps; a token or the forge's CLI login ([#76](https://github.com/dunkbing/imcode/issues/76))
- [ ] Project name and icon (image, emoji, monogram); pin, archive, remove ([#77](https://github.com/dunkbing/imcode/issues/77))
- [ ] Project overrides of settings, with where each value comes from ([#78](https://github.com/dunkbing/imcode/issues/78))
- [ ] Branch picker; base branch of a new worktree; a new thread in an existing worktree ([#79](https://github.com/dunkbing/imcode/issues/79))
- [ ] Worktree branch names (prefix or generated); worktree location; submodule depth ([#80](https://github.com/dunkbing/imcode/issues/80))
- [ ] Worktree cleanup rules: inactive, settled, merged, unchanged; deleted with its thread ([#81](https://github.com/dunkbing/imcode/issues/81))
- [ ] Copy-on-write worktree checkout on macOS ([#82](https://github.com/dunkbing/imcode/issues/82))
- [ ] Keep the default branch pulled when it can fast-forward ([#83](https://github.com/dunkbing/imcode/issues/83))
- [ ] Diff panel: file tree, split or unified, find, hide test files, one commit at a time ([#84](https://github.com/dunkbing/imcode/issues/84))
- [~] What each turn changed, as a line under it that opens its diffs (done); a changed-files tree ([#85](https://github.com/dunkbing/imcode/issues/85))
- [ ] Comment on lines of a diff or file; the comments go to the agent ([#86](https://github.com/dunkbing/imcode/issues/86))
- [ ] Generated commit messages, pull request titles and descriptions; repo conventions or custom instructions ([#87](https://github.com/dunkbing/imcode/issues/87))
- [ ] Pull requests page: list, filters, checks, timeline, comments, reviewers, labels, merge, auto-merge ([#88](https://github.com/dunkbing/imcode/issues/88))
- [ ] Review a pull request: line comments, files marked viewed, a generated guide through the change ([#89](https://github.com/dunkbing/imcode/issues/89))
- [ ] A thread from a pull request, in a worktree of its branch ([#90](https://github.com/dunkbing/imcode/issues/90))
- [ ] Pull requests linked to a thread, with a status badge; found from the branch ([#91](https://github.com/dunkbing/imcode/issues/91))
- [ ] An agent watches a pull request and wakes on checks, reviews, conflicts ([#92](https://github.com/dunkbing/imcode/issues/92))
- [ ] Stacked pull requests: move between layers, merge or rebase the stack ([#93](https://github.com/dunkbing/imcode/issues/93))
- [ ] Several terminals per thread, splits, scrollback kept on the host and restored on reconnect ([#94](https://github.com/dunkbing/imcode/issues/94))
- [ ] A terminal selection as context in the composer; terminal links open in the browser panel ([#95](https://github.com/dunkbing/imcode/issues/95))
- [~] File browser: tree, breadcrumbs, filter by file name (done; over sftp untested); quick open, text search across the project ([#96](https://github.com/dunkbing/imcode/issues/96))
- [ ] Edit and save files; previews of markdown, HTML, images, PDF, CSV, notebooks, office files, fonts ([#97](https://github.com/dunkbing/imcode/issues/97))
- [ ] Drag a file from the tree into the composer ([#98](https://github.com/dunkbing/imcode/issues/98))
- [ ] Browser panel: tabs, local servers found by port, zoom, a floating mini view ([#99](https://github.com/dunkbing/imcode/issues/99))
- [ ] The agent drives the browser; pick an element or annotate the page as context ([#100](https://github.com/dunkbing/imcode/issues/100))
- [ ] Browser profiles; cookies imported from an installed browser; the app as the default browser ([#101](https://github.com/dunkbing/imcode/issues/101))
- [ ] A browser on a remote host, shown here ([#102](https://github.com/dunkbing/imcode/issues/102))
- [ ] Project actions: named commands with keys, run in a terminal; run or stop when a thread settles ([#103](https://github.com/dunkbing/imcode/issues/103))
- [ ] Project config file in the repo: actions, workspace defaults ([#104](https://github.com/dunkbing/imcode/issues/104))
- [ ] Local servers of a thread listed with their ports; stop one ([#105](https://github.com/dunkbing/imcode/issues/105))
- [ ] Open the folder or a file in an external editor or file manager; a favorite with a key ([#106](https://github.com/dunkbing/imcode/issues/106))
- [ ] Device panel: a live iOS simulator or Android emulator beside the thread, for you and the agent ([#107](https://github.com/dunkbing/imcode/issues/107))
- [ ] Capture another window by a global key, attached to the draft with its accessibility text ([#108](https://github.com/dunkbing/imcode/issues/108))
- [ ] Right panel: tabs, maximize, resize ([#109](https://github.com/dunkbing/imcode/issues/109))
- [ ] Git staging area: stage and unstage files and hunks, discard changes; commit what is staged with the repo's hooks and signing ([#183](https://github.com/dunkbing/imcode/issues/183))
- [~] Git history: the commit list with a branch graph, and a commit's files and diff (done, click the branch under the composer); search by message, author or file; a commit's full message; the history of one file ([#184](https://github.com/dunkbing/imcode/issues/184))
- [ ] Git branches: list, create, switch, rename, delete; merge and rebase; ahead and behind; fetch and pull ([#185](https://github.com/dunkbing/imcode/issues/185))
- [ ] Git stash: save with a message, list, apply, pop, drop ([#186](https://github.com/dunkbing/imcode/issues/186))
- [ ] Git conflicts: conflicted files listed, take ours or theirs or edit; continue or abort the merge or rebase ([#187](https://github.com/dunkbing/imcode/issues/187))
- [ ] Git blame in the file viewer; jump from a line to its commit ([#188](https://github.com/dunkbing/imcode/issues/188))
- [ ] Git tags, cherry-pick, revert a commit, reset with a confirmation; interactive rebase ([#189](https://github.com/dunkbing/imcode/issues/189))
- [ ] Every git operation also works on an ssh project and never blocks the window ([#190](https://github.com/dunkbing/imcode/issues/190))

## 5. Phone (iOS and Android)

The phone is remote only: it runs no agent and keeps no project of its own. It reaches agents on other machines, either over ssh (the agent runs on that machine, as on the desktop) or as a client of a host (phase 3), which keeps the run going when the phone sleeps or loses its connection. Both platforms come from the same code through imshell's iOS and Android shells.

- [ ] iOS and Android builds from the same code; the narrow layout (thread list as a drawer) as the phone's layout ([#110](https://github.com/dunkbing/imcode/issues/110))
- [ ] Remote only: no local projects, terminal or agent installs; local-only features hidden ([#111](https://github.com/dunkbing/imcode/issues/111))
- [ ] Hosts: saved ssh hosts with keys kept in the Keychain (iOS) or Keystore (Android); add one by scanning a code from the desktop ([#112](https://github.com/dunkbing/imcode/issues/112))
- [ ] Threads over built-in ssh, as on the desktop ([#113](https://github.com/dunkbing/imcode/issues/113))
- [ ] Client of a host (phase 3): pick up the desktop's threads, live, and keep runs going when the phone sleeps ([#114](https://github.com/dunkbing/imcode/issues/114))
- [ ] Touch: long press for a message's actions, swipe a thread to archive, pull to refresh, the composer above the keyboard ([#115](https://github.com/dunkbing/imcode/issues/115))
- [ ] A notification when a thread needs an answer or ends (through the host, so it arrives while the app is closed) ([#116](https://github.com/dunkbing/imcode/issues/116))
- [ ] Answer a permission or question from the notification ([#117](https://github.com/dunkbing/imcode/issues/117))
- [ ] Voice input; haptics ([#118](https://github.com/dunkbing/imcode/issues/118))
- [ ] Ongoing agent activity: live activity on iOS, live update on Android, a home-screen widget ([#119](https://github.com/dunkbing/imcode/issues/119))
- [ ] Drafts, attachments and queued messages kept offline, sent on reconnect ([#120](https://github.com/dunkbing/imcode/issues/120))
- [ ] Photos and files from another app through the share sheet ([#121](https://github.com/dunkbing/imcode/issues/121))
- [ ] Diffs and changes readable on a phone: one file at a time, wrapped lines ([#122](https://github.com/dunkbing/imcode/issues/122))
- [ ] Pull request review on the phone, readable offline ([#123](https://github.com/dunkbing/imcode/issues/123))
- [ ] Tablet layout: a side inspector, hardware-keyboard shortcuts ([#124](https://github.com/dunkbing/imcode/issues/124))
- [ ] Wallpaper colors as a theme on Android ([#125](https://github.com/dunkbing/imcode/issues/125))
- [ ] Microphone preference order for dictation ([#126](https://github.com/dunkbing/imcode/issues/126))

## 6. Many agents

- [ ] Inbox: what needs an answer, what finished, across hosts ([#127](https://github.com/dunkbing/imcode/issues/127))
- [~] Desktop notifications when a thread finishes or needs an answer unseen (done, through the os helper); a dock badge ([#128](https://github.com/dunkbing/imcode/issues/128))
- [ ] One prompt to several agents or worktrees, results side by side ([#129](https://github.com/dunkbing/imcode/issues/129))
- [ ] Subagent threads shown under their parent ([#130](https://github.com/dunkbing/imcode/issues/130))
- [ ] MCP servers per project, passed to `session/new` ([#131](https://github.com/dunkbing/imcode/issues/131))
- [ ] Scheduled prompts ([#132](https://github.com/dunkbing/imcode/issues/132))
- [ ] Pin threads; reorder pinned and active by drag ([#133](https://github.com/dunkbing/imcode/issues/133))
- [ ] Snooze a thread until a time, with presets; it wakes early when it needs you ([#134](https://github.com/dunkbing/imcode/issues/134))
- [ ] Settle finished threads; auto-settle after inactivity or a merged pull request ([#135](https://github.com/dunkbing/imcode/issues/135))
- [ ] Working section: threads that are busy or waiting fold away until they need you ([#136](https://github.com/dunkbing/imcode/issues/136))
- [~] Sidebar grouped by project; a project filter (done) ([#137](https://github.com/dunkbing/imcode/issues/137))
- [ ] Undo for pin, settle, snooze, archive; act on several selected threads ([#138](https://github.com/dunkbing/imcode/issues/138))
- [ ] Search across hosts by title, branch, pull request, project and message text ([#139](https://github.com/dunkbing/imcode/issues/139))
- [ ] Thread row status: working, needs an answer, limited, pull request state; a hover card ([#140](https://github.com/dunkbing/imcode/issues/140))
- [ ] Archived threads list; delete a thread, with its worktree ([#141](https://github.com/dunkbing/imcode/issues/141))
- [ ] Drop files on a thread row to attach them ([#142](https://github.com/dunkbing/imcode/issues/142))
- [ ] Usage page: tokens, cache, estimated cost by agent and model; custom prices ([#143](https://github.com/dunkbing/imcode/issues/143))
- [ ] Subscription limits per account, in the composer and a widget ([#144](https://github.com/dunkbing/imcode/issues/144))
- [ ] New threads balanced across hosts ([#145](https://github.com/dunkbing/imcode/issues/145))
- [ ] Scheduled tasks: once or recurring, into a new thread each run or one thread; pause, run now ([#146](https://github.com/dunkbing/imcode/issues/146))
- [ ] Webhook tasks: a URL starts a run; placeholders, filters, signature check, dedupe, a delivery log ([#147](https://github.com/dunkbing/imcode/issues/147))
- [ ] Agents start and message other threads; stop reaches the subagents ([#148](https://github.com/dunkbing/imcode/issues/148))
- [ ] Notification settings per event; background thread events announced ([#149](https://github.com/dunkbing/imcode/issues/149))

## 7. Shipping

- [~] Settings: theme (done), font size, keys, default agent and permissions ([#150](https://github.com/dunkbing/imcode/issues/150))
- [x] ACP log view (every line in and out) ([#151](https://github.com/dunkbing/imcode/issues/151))
- [~] A signed, notarized macOS app (done, `scripts/package-macos.sh`, CI in `.github/workflows/macos.yml`), a Linux AppImage (done, `scripts/package-linux.sh`), builds on Windows (done); icon, Windows installer, CI on Windows and Linux ([#152](https://github.com/dunkbing/imcode/issues/152))
- [~] Auto-update (imshell's updater): Sparkle on macOS and the AppImage swap on Linux (done); staged installs on Windows, the feed and its key ([#153](https://github.com/dunkbing/imcode/issues/153))
- [~] Welcome page on first run: agents found, projects to import, phone setup ([#154](https://github.com/dunkbing/imcode/issues/154))
- [~] Command palette: threads, projects, actions, themes, add project ([#155](https://github.com/dunkbing/imcode/issues/155))
- [ ] Keybindings editor: rules with when-conditions, a config file, conflicts warned ([#156](https://github.com/dunkbing/imcode/issues/156))
- [ ] Keyboard focus moves between sidebar, transcript, composer and panels; keys in the thread list ([#157](https://github.com/dunkbing/imcode/issues/157))
- [ ] Themes: a light and dark pair, a theme editor, import, a theme per host ([#158](https://github.com/dunkbing/imcode/issues/158))
- [ ] Typography: UI and code fonts, sizes; reduced motion ([#159](https://github.com/dunkbing/imcode/issues/159))
- [ ] Search in settings ([#160](https://github.com/dunkbing/imcode/issues/160))
- [ ] Text model for titles, commit and pull request text: an agent or an API endpoint with a key ([#161](https://github.com/dunkbing/imcode/issues/161))
- [ ] Secret providers: credentials by reference from a password manager ([#162](https://github.com/dunkbing/imcode/issues/162))
- [ ] Stable and nightly tracks; release notes; update app, hosts and agents in one step ([#163](https://github.com/dunkbing/imcode/issues/163))
- [ ] Diagnostics: connection checks, resource use, export ([#164](https://github.com/dunkbing/imcode/issues/164))
- [ ] Hold to quit; reopen a closed view ([#165](https://github.com/dunkbing/imcode/issues/165))
- [ ] Package managers: Homebrew, winget, scoop, deb, AUR, nix ([#166](https://github.com/dunkbing/imcode/issues/166))
- [ ] Open-source licenses page ([#167](https://github.com/dunkbing/imcode/issues/167))
