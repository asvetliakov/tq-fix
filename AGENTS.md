# Git commands

Use `git --no-pager` for Git inspection commands so interactive pagers cannot
block execution. In particular, run `git --no-pager diff --check`,
`git --no-pager diff`, `git --no-pager log`, and `git --no-pager show`.

An empty result from `git --no-pager diff --check` with exit code 0 means the
whitespace check passed.

# Workflow

Carry authorized work through investigation, fixes, verification, documentation,
local test setup and commits without asking for repeated confirmation. Preserve
the previous DLL, configuration and logs before preparing a new gameplay test.
