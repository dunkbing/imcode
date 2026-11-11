# Roadmap

imcode runs coding agents (Claude Code, Codex, Gemini, any ACP agent) in folders on this machine or on machines reached over ssh, several at once, from desktop and phone.

Order is by phase; within a phase, top first. `[x]` done, `[~]` partly.

## 1. Chat

- [x] Threads: an agent in a folder, several running at once, saved across launches and resumed with `session/load`
- [x] Transcript: messages, thoughts, tool calls, plans; permission and sign-in prompts
- [x] Model, mode and effort pickers from the agent's config options, there before the first message
- [~] Markdown (done, imgui_rich_md) and highlighted code blocks
- [x] Fonts and icons: JetBrains Mono, an icon font, the system CJK font
- [ ] Slash commands and `@file` mentions in the composer
- [x] Tool call details: diffs for edits, output for commands, files that open in the viewer
- [ ] Agents install themselves: managed Bun and registry binaries, with progress
- [~] Queue a message while a turn runs (done); edit and resend the last one
- [ ] Images and files as attachments (paste, drop)
- [ ] Context and cost from `usage_update`; compaction notices
- [~] Rename, archive, search threads
- [~] Detect installed agents: turn on, install, sign in, update; browse the ACP registry
- [ ] Add a local ACP command as an agent; several accounts or configs of one agent
- [~] Import projects and recent threads from agent CLI history (Claude Code and Codex done, Pi)
- [ ] Custom models per agent; model, effort and mode remembered for new threads
- [ ] Permission mode per thread: supervised, auto-accept edits, auto, full access; a default for new threads
- [ ] Approve or reject requests in the transcript; a subagent's requests asked in the parent
- [ ] Agent questions: pick an option or type an answer, with files attached
- [ ] Plan mode: a proposed plan card to accept or revise; task lists
- [ ] Steer the running turn or queue, per message; send a queued message now
- [ ] Queued messages held after a stop or restart, with resume
- [ ] Interrupt a turn; force-stop an unresponsive one
- [ ] `$skill` mentions; skills in the slash menu; restart the agent session after changing skills or MCP
- [ ] `/compact`, `/clear` (a fresh session in the same thread), `/goal` with progress
- [ ] `@thread` mentions and thread links as context; drag a thread onto the composer
- [ ] `@model` mentions to name a model when delegating; `#` to mention a pull request
- [ ] Context chips inline in the message: terminal excerpt, diff comment, file, picked element
- [ ] Rich-text markdown composer with code blocks; a plain mode
- [ ] Large pastes become text-file attachments
- [ ] Quote part of a reply into the composer, with a comment
- [ ] Recall sent prompts with up and down; stash a draft and restore it later
- [ ] Suggested next prompt after a turn
- [ ] Edit an earlier prompt: rewind the conversation, keep or revert the files
- [ ] Fork a thread; switch agent mid-thread with the context handed over
- [ ] Side chat: ask about a thread without adding to it, bring the answer back
- [ ] Voice dictation in the composer: on this machine or a host, with transcript cleanup
- [ ] Send key: enter or mod+enter; send and start the next thread in the background
- [ ] Tool calls grouped into an expandable summary; hide successful or all; a mode with only the last reply
- [ ] Live output of a running command
- [ ] Find in thread; a minimap of the timeline
- [ ] Copy a message as markdown; copy a link to a thread
- [ ] Math, mermaid diagrams, tables, images and video in replies; image zoom
- [ ] Visual replies: an agent's HTML page rendered sandboxed in the thread; MCP apps
- [ ] Preview attached files: code, markdown, CSV, PDF, audio, zip
- [ ] Status line: elapsed time, goal, errors; a details card with context, usage, servers, pull requests
- [ ] Usage-limit stops: resume at reset, snooze until reset
- [ ] Compact idle threads automatically (Claude Code)
- [ ] Generated thread titles; regenerate; title instructions
- [ ] A thread without a project, in a scratch folder; turn it into a project
- [ ] Secret requests: an agent asks for a secret in a private card

## 2. Remote over ssh

The agent runs on the remote machine in the remote folder; nothing is installed there but the agent.

- [x] Built-in ssh (libssh2), one connection per host: the ssh agent, `~/.ssh/config`, default keys, a password or passphrase; new host keys are remembered, a changed one is refused
- [x] Open folder: pick a host (from `~/.ssh/config` or typed), then browse its folders
- [x] A thread in a remote folder: the agent runs there with the login shell's `PATH`
- [ ] Existing sessions of a host and folder (`session/list`): continue what was started in a terminal
- [ ] Sign in to an agent on the remote (a terminal over ssh)
- [ ] Reconnect with resume after a drop or sleep, without freezing the window
- [ ] Install an agent on the remote (bun or a registry binary) from the app
- [ ] Passwords and passphrases in the system keychain; Windows sockets
- [ ] Runs that survive a disconnect: a small host process on the remote (phase 3) that the app installs and attaches to
- [ ] Host picker on a new thread; cycle hosts with a key
- [ ] Add a project that exists on one host to another host
- [ ] Saved hosts with a name and icon; a status dot and a connection diagnosis
- [ ] A remote local server opened in the browser panel through a forward

## 3. Host and sync

One host per machine owns the agents and the threads; every screen is a client of it.

- [x] Agent logic off the frame loop: a host thread polls the agents, also while the window is in the background
- [ ] Commands in, events out, as JSON; in-process on desktop
- [ ] Thread entries in SQLite with row versions; a client that reconnects fetches what changed
- [ ] `imcode --serve`: the host without a window, on a Linux box or a Mac
- [ ] Links: a channel of the built-in ssh to the host's loopback port; WebSocket with a pairing code on a LAN or tailnet
- [ ] Several clients on one thread (desktop and phone)
- [ ] The host as a background service (launchd, systemd): install, status, restart
- [ ] Pair by QR code or link; list and revoke paired clients
- [ ] One host reachable by several routes (LAN, tailnet, ssh), the working one picked
- [ ] HTTPS on a tailnet
- [ ] Linked hosts: threads on one referenced from another
- [ ] Settings kept on the host; edit one host or all connected hosts at once
- [ ] Update a connected host from a client; a warning when versions differ
- [ ] Guest link: someone follows one thread in a browser and proposes messages you send or dismiss
- [ ] Prompt links: a URL that opens a draft with a prompt, repo and model, never sent by itself
- [ ] The host as an MCP server: outside agents read, start and message threads, with a scoped sign-in
- [ ] `imcode <folder>` opens a project from a terminal; thread ids for scripts
- [ ] Storage page: database and attachment size, reclaim space

## 4. Workspace

- [~] Projects: threads grouped by host and folder (done), with the git branch
- [ ] A git worktree per thread, with a setup command
- [ ] Diff view: what a turn changed, and the working tree against its base
- [ ] Checkpoints as hidden git refs; revert the files of a turn
- [ ] Commit, push, open a pull request
- [~] Terminal in the thread's folder, local or remote (libghostty); no selection or copy yet
- [~] File viewer: read-only, line numbers, simple syntax colours; no selection or search in the file yet
- [~] Add project: a local folder, a new git repo, a clone from a URL or a forge; the clone runs in the background
- [ ] Publish a local repo to a forge
- [ ] Forge accounts: GitHub, GitLab, Forgejo/Gitea, Bitbucket, Azure DevOps; a token or the forge's CLI login
- [ ] Project name and icon (image, emoji, monogram); pin, archive, remove
- [ ] Project overrides of settings, with where each value comes from
- [ ] Branch picker; base branch of a new worktree; a new thread in an existing worktree
- [ ] Worktree branch names (prefix or generated); worktree location; submodule depth
- [ ] Worktree cleanup rules: inactive, settled, merged, unchanged; deleted with its thread
- [ ] Copy-on-write worktree checkout on macOS
- [ ] Keep the default branch pulled when it can fast-forward
- [ ] Diff panel: file tree, split or unified, find, hide test files, one commit at a time
- [ ] Changed-files tree under each turn
- [ ] Comment on lines of a diff or file; the comments go to the agent
- [ ] Generated commit messages, pull request titles and descriptions; repo conventions or custom instructions
- [ ] Pull requests page: list, filters, checks, timeline, comments, reviewers, labels, merge, auto-merge
- [ ] Review a pull request: line comments, files marked viewed, a generated guide through the change
- [ ] A thread from a pull request, in a worktree of its branch
- [ ] Pull requests linked to a thread, with a status badge; found from the branch
- [ ] An agent watches a pull request and wakes on checks, reviews, conflicts
- [ ] Stacked pull requests: move between layers, merge or rebase the stack
- [ ] Several terminals per thread, splits, scrollback kept on the host and restored on reconnect
- [ ] A terminal selection as context in the composer; terminal links open in the browser panel
- [~] File browser: tree, breadcrumbs, filter by file name (done; over sftp untested); quick open, text search across the project
- [ ] Edit and save files; previews of markdown, HTML, images, PDF, CSV, notebooks, office files, fonts
- [ ] Drag a file from the tree into the composer
- [ ] Browser panel: tabs, local servers found by port, zoom, a floating mini view
- [ ] The agent drives the browser; pick an element or annotate the page as context
- [ ] Browser profiles; cookies imported from an installed browser; the app as the default browser
- [ ] A browser on a remote host, shown here
- [ ] Project actions: named commands with keys, run in a terminal; run or stop when a thread settles
- [ ] Project config file in the repo: actions, workspace defaults
- [ ] Local servers of a thread listed with their ports; stop one
- [ ] Open the folder or a file in an external editor or file manager; a favorite with a key
- [ ] Device panel: a live iOS simulator or Android emulator beside the thread, for you and the agent
- [ ] Capture another window by a global key, attached to the draft with its accessibility text
- [ ] Right panel: tabs, maximize, resize

## 5. Phone

- [ ] iOS and Android builds; a narrow layout (thread list as a drawer)
- [ ] Remote threads over built-in ssh, and as a client of a host
- [ ] A notification when a thread needs an answer or ends
- [ ] Voice input; haptics
- [ ] Phone setup from the desktop: scan a code to install and pair
- [ ] Ongoing agent activity: live activity on iOS, live update on Android, a home-screen widget
- [ ] Drafts, attachments and queued messages kept offline, sent on reconnect
- [ ] Files from another app through the share sheet
- [ ] Arrange threads by drag in a sheet
- [ ] Pull request review on the phone, readable offline
- [ ] Tablet layout: a side inspector, hardware-keyboard shortcuts
- [ ] Wallpaper colors as a theme on Android
- [ ] Microphone preference order for dictation

## 6. Many agents

- [ ] Inbox: what needs an answer, what finished, across hosts
- [~] Desktop notifications when a thread finishes or needs an answer unseen (done, through the os helper); a dock badge
- [ ] One prompt to several agents or worktrees, results side by side
- [ ] Subagent threads shown under their parent
- [ ] MCP servers per project, passed to `session/new`
- [ ] Scheduled prompts
- [ ] Pin threads; reorder pinned and active by drag
- [ ] Snooze a thread until a time, with presets; it wakes early when it needs you
- [ ] Settle finished threads; auto-settle after inactivity or a merged pull request
- [ ] Working section: threads that are busy or waiting fold away until they need you
- [ ] Sidebar grouped by project; a project filter
- [ ] Undo for pin, settle, snooze, archive; act on several selected threads
- [ ] Search across hosts by title, branch, pull request, project and message text
- [ ] Thread row status: working, needs an answer, limited, pull request state; a hover card
- [ ] Archived threads list; delete a thread, with its worktree
- [ ] Drop files on a thread row to attach them
- [ ] Usage page: tokens, cache, estimated cost by agent and model; custom prices
- [ ] Subscription limits per account, in the composer and a widget
- [ ] New threads balanced across hosts
- [ ] Scheduled tasks: once or recurring, into a new thread each run or one thread; pause, run now
- [ ] Webhook tasks: a URL starts a run; placeholders, filters, signature check, dedupe, a delivery log
- [ ] Agents start and message other threads; stop reaches the subagents
- [ ] Notification settings per event; background thread events announced

## 7. Shipping

- [ ] Settings: theme, font size, keys, default agent and permissions
- [ ] ACP log view (every line in and out)
- [ ] App bundle, icon, installers; CI on macOS, Windows, Linux
- [ ] Auto-update (imshell's updater)
- [~] Welcome page on first run: agents found, projects to import, phone setup
- [~] Command palette: threads, projects, actions, themes, add project
- [ ] Keybindings editor: rules with when-conditions, a config file, conflicts warned
- [ ] Keyboard focus moves between sidebar, transcript, composer and panels; keys in the thread list
- [ ] Themes: a light and dark pair, a theme editor, import, a theme per host
- [ ] Typography: UI and code fonts, sizes; reduced motion
- [ ] Search in settings
- [ ] Text model for titles, commit and pull request text: an agent or an API endpoint with a key
- [ ] Secret providers: credentials by reference from a password manager
- [ ] Stable and nightly tracks; release notes; update app, hosts and agents in one step
- [ ] Diagnostics: connection checks, resource use, export
- [ ] Hold to quit; reopen a closed view
- [ ] Package managers: Homebrew, winget, scoop, deb, AUR, nix
- [ ] Open-source licenses page
