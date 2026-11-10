# Roadmap

imcode runs coding agents (Claude Code, Codex, Gemini, any ACP agent) in folders on this machine or on machines reached over ssh, several at once, from desktop and phone.

Order is by phase; within a phase, top first. `[x]` done, `[~]` partly.

## 1. Chat

- [x] Threads: an agent in a folder, several running at once, saved across launches and resumed with `session/load`
- [x] Transcript: messages, thoughts, tool calls, plans; permission and sign-in prompts
- [x] Model, mode and effort pickers from the agent's config options, there before the first message
- [ ] Markdown and highlighted code blocks (imgui_rich_md, as DearSQL)
- [ ] Fonts and icons (JetBrains Mono, an icon font, CJK)
- [ ] Slash commands and `@file` mentions in the composer
- [ ] Tool call details: diffs for edits, output for commands, file links
- [ ] Agents install themselves: managed Bun and registry binaries, with progress
- [ ] Queue a message while a turn runs; edit and resend the last one
- [ ] Images and files as attachments (paste, drop)
- [ ] Context and cost from `usage_update`; compaction notices
- [ ] Rename, archive, search threads

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

## 3. Host and sync

One host per machine owns the agents and the threads; every screen is a client of it.

- [ ] Agent logic off the frame loop (today a background window delays a sign-in restart)
- [ ] Commands in, events out, as JSON; in-process on desktop
- [ ] Thread entries in SQLite with row versions; a client that reconnects fetches what changed
- [ ] `imcode --serve`: the host without a window, on a Linux box or a Mac
- [ ] Links: a channel of the built-in ssh to the host's loopback port; WebSocket with a pairing code on a LAN or tailnet
- [ ] Several clients on one thread (desktop and phone)

## 4. Workspace

- [~] Projects: threads grouped by host and folder (done), with the git branch
- [ ] A git worktree per thread, with a setup command
- [ ] Diff view: what a turn changed, and the working tree against its base
- [ ] Checkpoints as hidden git refs; revert the files of a turn
- [ ] Commit, push, open a pull request
- [ ] Terminal in the thread's folder, local or remote
- [ ] File viewer with search

## 5. Phone

- [ ] iOS and Android builds; a narrow layout (thread list as a drawer)
- [ ] Remote threads over built-in ssh, and as a client of a host
- [ ] A notification when a thread needs an answer or ends
- [ ] Voice input; haptics

## 6. Many agents

- [ ] Inbox: what needs an answer, what finished, across hosts
- [ ] Desktop notifications and a dock badge
- [ ] One prompt to several agents or worktrees, results side by side
- [ ] Subagent threads shown under their parent
- [ ] MCP servers per project, passed to `session/new`
- [ ] Scheduled prompts

## 7. Shipping

- [ ] Settings: theme, font size, keys, default agent and permissions
- [ ] ACP log view (every line in and out)
- [ ] App bundle, icon, installers; CI on macOS, Windows, Linux
- [ ] Auto-update (mydear's updater)
