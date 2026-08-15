#!/usr/bin/env bash
# Runs once after the container is created, as the `vscode` user.
set -euo pipefail

ME="$(id -un)"

# --- 1. Bind mounts can arrive owned by root (first-ever creation on the host,
#        or a UID remap). Fix only when needed so rebuilds stay fast.
for d in "$HOME/.claude"; do
  mkdir -p "$d"
  if [ "$(stat -c '%U' "$d")" != "$ME" ]; then
    sudo chown -R "$ME:$ME" "$d"
  fi
done

# --- 2. Shell environment
BASHRC="$HOME/.bashrc"
if ! grep -q '# >>> devcontainer >>>' "$BASHRC" 2>/dev/null; then
  cat >> "$BASHRC" <<'EOF'

# >>> devcontainer >>>
export PATH="$HOME/.local/bin:$PATH"

alias b='cmake --build build/$(basename "$(readlink -f build/.preset 2>/dev/null)" 2>/dev/null || echo linux-gcc16-cxx26)'
# <<< devcontainer <<<
EOF
fi

# --- 3. Report what we actually got, so a bad version pin is obvious on day one
echo "-------------------------------------------------------------"
gcc --version | head -1
clang --version | head -1
cmake --version | head -1
echo "ninja $(ninja --version)"
echo "node  $(node --version 2>/dev/null || echo 'missing')"
command -v claude >/dev/null && echo "claude $(claude --version 2>/dev/null || echo installed)" || echo "claude: MISSING"
command -v docker >/dev/null && (docker version --format '{{.Server.Version}}' >/dev/null 2>&1 \
  && echo "docker: host daemon reachable" \
  || echo "docker: CLI present but daemon NOT reachable; check Docker Desktop WSL integration")
echo "-- C++ standards this g++ accepts:"
g++ -v --help 2>/dev/null | grep -Eo '\-std=c\+\+[0-9a-z]+' | sort -u | tr '\n' ' '; echo
echo "-- C++ standards this clang++ accepts:"
clang++ -std=blah -xc++ /dev/null 2>&1 | grep -Eo "'c\+\+[0-9a-z]+'" | sort -u | tr '\n' ' '; echo
echo "-------------------------------------------------------------"
