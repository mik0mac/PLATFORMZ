# Redeploy after code changes

Commands only - the reasoning is in `deploy-vultr.md`. The box pulls `main`, so
nothing here goes live until its PR is merged.

## On your Mac

- `./rebuild-all.sh`
- `./test/run.sh`
- `./server/test/run_all.sh`
- `./server/test/run_probes.sh`
- `git status` - client changes must include a rebuilt `web/`; the box serves it from the checkout
- `git add -A && git commit -m "..."` (on a branch)
- `git push -u origin <branch>`
- `gh pr create` - then wait for CI to be green, including **Web client** (bundle fresh) and **CMake Windows**
- `gh pr merge <n> --merge`

## On the box

- `ssh mik0mac@platformz.space`
- `cd /opt/PLATFORMZ`
- `git checkout main && git pull`
- `make -C server`
- `sudo systemctl restart platformz`
- `sudo cp /opt/PLATFORMZ/web/platformz.* /var/www/platformz/`

## Verify

- `sudo systemctl status platformz`
- `sudo journalctl -u platformz -n 20`
- `curl -s http://platformz.space:9000/status`
- `curl -s https://platformz.space/platformz.wasm | shasum`
- `shasum /opt/PLATFORMZ/web/platformz.wasm`

## Desktop builds (only when the client changed)

A desktop player on an older build than the server gets SERVER VERSION MISMATCH,
so hand out fresh builds with any protocol change.

- Mac: `make dist-pack` -> `dist/PLATFORMZ-mac-arm64.zip`
- Windows: `gh run download <run-id> -n platformz-windows` (the merged commit's CI run) -> `PLATFORMZ-UNSIGNED-windows-x64.zip`
- Steam: nothing to upload yet (no App ID, #95)

## Roll back

- `cd /opt/PLATFORMZ && git log --oneline -5`
- `git checkout <sha>`
- `make -C server && sudo systemctl restart platformz`
- `sudo cp /opt/PLATFORMZ/web/platformz.* /var/www/platformz/`
- `git checkout main` - when the fix is merged, so the next `git pull` lands on a branch
