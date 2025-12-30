# Contributing & Branch Workflow

A lightweight workflow to keep changes organized and avoid losing fixes.

## Branches
- `main`: Stable releases (protected; PR-only)
- `dev`: Integration branch (protected; PR-only)
- `feature/*`: New work (e.g., `feature/nat-router`)
- `hotfix/*`: Quick production fixes
- `archive/*`: Snapshot branches for imported histories (e.g., `archive/old-dev-2025-12-27`)

## Flow
1. Create a feature branch from `dev`.
2. Commit in small, focused chunks (use conventional types).
3. Open a PR to `dev` with the template.
4. CI/build check + review, then squash or rebase merge.
5. Release by PR `dev -> main`, tag a version.

## Commit Message Convention
Use one of:
- `feat: ...` new functionality
- `fix: ...` bug fix
- `perf: ...` performance improvement
- `security: ...` security fix/hardening
- `refactor: ...` code-only restructure
- `docs: ...` documentation
- `chore: ...` tooling/config

Write clear, user-impacting subjects and include a short body for context when useful.

## PR Checklist (Quick)
- Build passes: `pio run -e waveshare-349`
- sdkconfig defaults up to date (e.g., NAT/NAPT)
- README updated for user-facing changes
- Web UI output escaped; passwords obfuscated
- No secrets or junk files committed

## Recovering Missed Fixes
When histories diverge (like recent force-push):
1. Create an archive branch pointing to the old commit (example already added: `archive/old-dev-2025-12-27`).
2. Diff `archive/*` vs `dev` to identify functional changes worth porting.
3. Implement the changes in a new `recovery/port-missed-fixes` branch as minimal commits.
4. Open a PR to `dev` and review.

## Branch Protection (GitHub settings)
Recommend enabling:
- Require PRs to merge to `main` and `dev`.
- Require status checks (build/size check).
- Prevent force-pushes to protected branches.
- Require at least one review approval.

## Local Quality Checks
- `pio run -e waveshare-349` (build)
- Optional: `pio run -e waveshare-349 -t upload` (device test)
- Review diffs for user-facing strings and HTML outputs.

Thanks for contributing!
