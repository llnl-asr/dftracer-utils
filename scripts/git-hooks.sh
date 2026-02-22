#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(git -C "$SCRIPT_DIR/.." rev-parse --show-toplevel)"
HOOKS_DIR="$(git -C "$REPO_ROOT" rev-parse --git-path hooks)"

usage() {
    cat <<'EOF'
Usage: scripts/git-hooks.sh <command>

Commands:
  install        Install repository git hooks (pre-commit, commit-msg, pre-push)
  uninstall      Remove repository git hooks installed by this script
  run-pre-commit Run pre-commit checks (format + check-format)
  run-commit-msg Validate commit message file passed as argument
  run-pre-push   Run pre-push checks
  help           Show this help message
EOF
}

backup_hook_if_exists() {
    local hook_path="$1"
    if [ -e "$hook_path" ] && ! grep -q "dftracer-utils managed hook" "$hook_path"; then
        cp "$hook_path" "${hook_path}.bak"
    fi
}

write_pre_commit_hook() {
    local hook_path="$HOOKS_DIR/pre-commit"
    backup_hook_if_exists "$hook_path"

    cat >"$hook_path" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
# dftracer-utils managed hook
repo_root="$(git rev-parse --show-toplevel)"
"$repo_root/scripts/git-hooks.sh" run-pre-commit
EOF
    chmod +x "$hook_path"
}

write_commit_msg_hook() {
    local hook_path="$HOOKS_DIR/commit-msg"
    backup_hook_if_exists "$hook_path"

    cat >"$hook_path" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
# dftracer-utils managed hook
repo_root="$(git rev-parse --show-toplevel)"
"$repo_root/scripts/git-hooks.sh" run-commit-msg "$1"
EOF
    chmod +x "$hook_path"
}

write_pre_push_hook() {
    local hook_path="$HOOKS_DIR/pre-push"
    backup_hook_if_exists "$hook_path"

    cat >"$hook_path" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
# dftracer-utils managed hook
repo_root="$(git rev-parse --show-toplevel)"
"$repo_root/scripts/git-hooks.sh" run-pre-push
EOF
    chmod +x "$hook_path"
}

install_hooks() {
    mkdir -p "$HOOKS_DIR"
    write_pre_commit_hook
    write_commit_msg_hook
    write_pre_push_hook
    echo "Installed hooks in $HOOKS_DIR"
}

remove_managed_hook() {
    local hook_path="$1"
    if [ -f "$hook_path" ] && grep -q "dftracer-utils managed hook" "$hook_path"; then
        rm -f "$hook_path"
    fi
}

uninstall_hooks() {
    remove_managed_hook "$HOOKS_DIR/pre-commit"
    remove_managed_hook "$HOOKS_DIR/commit-msg"
    remove_managed_hook "$HOOKS_DIR/pre-push"
    echo "Removed managed hooks from $HOOKS_DIR"
}

run_pre_commit() {
    echo "[pre-commit] running format"
    make -C "$REPO_ROOT" format

    if ! git -C "$REPO_ROOT" diff --quiet -- src include tests; then
        echo "[pre-commit] formatting changed files. Stage them and re-run commit."
        git -C "$REPO_ROOT" --no-pager diff -- src include tests
        exit 1
    fi

    echo "[pre-commit] checking format"
    make -C "$REPO_ROOT" check-format
}

run_commit_msg() {
    local msg_file="${1:-}"
    if [ -z "$msg_file" ] || [ ! -f "$msg_file" ]; then
        echo "[commit-msg] missing commit message file"
        exit 1
    fi

    local first_line
    first_line="$(grep -vE '^[[:space:]]*(#|$)' "$msg_file" | head -n 1 | tr -d '\r')"
    if [ -z "$first_line" ]; then
        echo "[commit-msg] commit message cannot be empty"
        exit 1
    fi
    if printf '%s' "$first_line" | grep -Eiq '^wip\b'; then
        echo "[commit-msg] WIP commit messages are not allowed"
        exit 1
    fi
}

run_pre_push() {
    echo "[pre-push] checking format"
    make -C "$REPO_ROOT" check-format
}

main() {
    local cmd="${1:-help}"
    case "$cmd" in
        install)
            install_hooks
            ;;
        uninstall)
            uninstall_hooks
            ;;
        run-pre-commit)
            run_pre_commit
            ;;
        run-commit-msg)
            shift
            run_commit_msg "$@"
            ;;
        run-pre-push)
            run_pre_push
            ;;
        help|-h|--help)
            usage
            ;;
        *)
            echo "Unknown command: $cmd"
            usage
            exit 1
            ;;
    esac
}

main "$@"
