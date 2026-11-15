#!/bin/sh
# LinuxRunner Agent Mode launcher (installed as /usr/local/bin/lr-agent).
#   lr-agent            menu of the installed coding agents
#   lr-agent <command>  start that agent in ~/Projects
# Each agent asks for its own sign-in the first time; nothing is stored by
# LinuxRunner.
projects="$HOME/Projects"
mkdir -p "$projects" 2>/dev/null && cd "$projects" || cd "$HOME" || exit 1
export TERM="${TERM:-xterm-256color}" COLORTERM="${COLORTERM:-truecolor}"
if [ -n "${1:-}" ]; then exec "$@"; fi
have() { command -v "$1" >/dev/null 2>&1; }
printf '\n\033[1;35m  LinuxRunner Agent Mode\033[0m\n'
printf '\033[2m  Projects folder: %s\033[0m\n\n' "$PWD"
n=0; list=
for entry in "claude:Claude Code (Anthropic)" "codex:Codex CLI (OpenAI)" "opencode:OpenCode"; do
    command=${entry%%:*}; title=${entry#*:}
    have "$command" || continue
    n=$((n + 1)); list="$list $command"
    printf '  \033[1;36m%d\033[0m  %s\n' "$n" "$title"
done
printf '  \033[1;36ms\033[0m  Shell in this folder\n\n'
[ "$n" -gt 0 ] || printf '  No agents are installed yet. Install Agent Mode in Settings > Modes.\n\n'
printf '  Choose: '
read -r choice || exit 0
case "$choice" in
    s|S|'') exec "${SHELL:-/bin/sh}" -l;;
esac
i=0
for command in $list; do
    i=$((i + 1))
    if [ "$choice" = "$i" ] || [ "$choice" = "$command" ]; then
        "$command"; status=$?
        printf '\n\033[2m  %s exited (%s). Shell in %s; type lr-agent for the menu.\033[0m\n' "$command" "$status" "$PWD"
        exec "${SHELL:-/bin/sh}" -l
    fi
done
printf '  Unknown choice. Type lr-agent to try again.\n'
exec "${SHELL:-/bin/sh}" -l
