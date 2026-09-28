# Redeploy after code changes

## On your Mac

- `./rebuild-all.sh`
- `./test/run.sh`
- `./server/test/run_all.sh`
- `./server/test/run_probes.sh`
- `git add -A && git commit -m "..."`
- `git push`

## On the box

- `ssh mik0mac@platformz.space`
- `cd /opt/PLATFORMZ`
- `git pull`
- `make -C server`
- `sudo systemctl restart platformz`
- `sudo cp /opt/PLATFORMZ/web/platformz.* /var/www/platformz/`

## Verify

- `sudo systemctl status platformz`
- `sudo journalctl -u platformz -n 20`
- `curl -s http://platformz.space:9000/status`
- `curl -s https://platformz.space/platformz.wasm | shasum`
- `shasum /opt/PLATFORMZ/web/platformz.wasm`

## Roll back

- `cd /opt/PLATFORMZ && git log --oneline -5`
- `git checkout <sha>`
- `make -C server && sudo systemctl restart platformz`
