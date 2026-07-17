Short answer: no, you wouldn't need to open any port, and it wouldn't interfere with the gameserver.

The reason is that a Discord bot works backwards from what people expect. It doesn't sit there waiting for Discord to call it — it dials out to Discord's gateway and holds a persistent WebSocket open, then receives events down that connection it already established. That's an outbound connection, and ufw only filters inbound traffic by default. Outbound is unrestricted, and replies on a connection you initiated come back automatically because the firewall is stateful — it remembers you opened that conversation. So the bot just works, with the firewall untouched.

This is the same reason your gameserver needed ufw allow 9000 but your git pull didn't: the gameserver listens for strangers, git and the bot call out.

On interference: none, because the bot binds no port at all. Two programs only collide when they try to listen on the same port — exactly what bit us today, twice (nginx vs Caddy on :80, then the stray Caddy vs the real one on :2019). A bot that only dials out has nothing to collide with.

The one thing to watch is resources rather than ports. You're on the 1 vCPU / 1 GB plan, and your gameserver runs a 60 Hz simulation loop — it wants its CPU on a steady heartbeat. A typical Node or Python bot idles at maybe 50–150 MB and near-zero CPU, so there's plenty of headroom. But if the bot ever does something heavy (transcoding audio for a music bot is the classic), it could cause frame hitches for players. Worth knowing before you add one, not worth worrying about for an ordinary command bot.

Two cases that would change the answer:

If you use Discord's Interactions Endpoint URL instead of the gateway, Discord flips the direction and POSTs to you over HTTPS. Even then you wouldn't open a new port — you'd let Caddy handle it on 443, which is already open, by adding a route or a subdomain block to the Caddyfile. Same for a web dashboard.

And if the bot ships a health-check or metrics port (some frameworks default to something like 8080), that does listen — but it should bind 127.0.0.1 only, never 0.0.0.0, so it stays reachable from the box and invisible to the internet. No ufw rule, no exposure.

When you get to it, the pattern that fits this box: its own systemd unit alongside platformz.service, with the bot token in a root-only /etc/discordbot.env via EnvironmentFile — mirroring exactly how PLATFORMZ_KEY is handled today, so the token never touches the repo. Say the word and I'll plan that out properly.