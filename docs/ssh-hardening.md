# SSH hardening on the Vultr server (key-only logins)

Every server with a public IP gets hammered around the clock by bots guessing
passwords for `root`, `admin`, `user`, etc. — that's the wall of
`Failed password` noise in `/var/log/auth.log`. None of it works, but the door
they're knocking on doesn't need to exist: keys get us in. This walkthrough
turns off password logins entirely, so every bot attempt is rejected instantly
(`Permission denied (publickey)`) before a password is ever asked.

This was done on the PLATFORMZ server on 2026-07-11. Replace `SERVER_IP` with
the server's public IP throughout.

**Golden rule: never close your working SSH session until you've proven the
new setup works in a second terminal window.** If anything goes wrong, you're
still inside and can undo it.

## Step 1 — Give your key to the admin user (from your Mac)

The `mik0mac` user needs to trust your key, not just root. On your Mac:

```
ssh-copy-id mik0mac@SERVER_IP
```

It asks for mik0mac's password — the last time you should ever need to type
it over SSH. Behind the scenes this pastes your **public** key into
`/home/mik0mac/.ssh/authorized_keys` on the server (the list of keys allowed
in) with the correct permissions.

## Step 2 — Prove it worked

```
ssh mik0mac@SERVER_IP
```

Should land at a prompt **with no password asked**. While in there, confirm
sudo works:

```
sudo whoami
```

sudo still asks for mik0mac's password (that's separate from SSH and fine);
it should print `root`. **Keep this session open from here on.**

## Step 3 — Turn off password logins (on the server)

Create a small config file that overrides the defaults:

```
sudo nano /etc/ssh/sshd_config.d/00-hardening.conf
```

Contents (three lines):

```
PasswordAuthentication no
KbdInteractiveAuthentication no
PermitRootLogin prohibit-password
```

- Line 1 turns off normal password logins.
- Line 2 turns off a second flavor of password prompt that would otherwise
  still work.
- Line 3 lets root log in with a key but never a password.

Why a new file instead of editing `/etc/ssh/sshd_config`: Ubuntu reads extra
settings from the `sshd_config.d` folder, and Vultr drops in its own file
there (`50-cloud-init.conf`) that switches passwords **on**. Files are read in
name order and **the first setting wins**, so naming ours `00-...` guarantees
it beats Vultr's `50-...`.

## Step 4 — Check the config, then restart SSH

```
sudo sshd -t
```

No output means the config is valid. If it prints an error, fix the typo it
names — **don't restart on a broken config**.

```
sudo systemctl restart ssh
```

Restarting SSH does not kick your current session — only new connections are
affected.

## Step 5 — Verify, in a NEW terminal window

Don't close the old one yet. All three must pass:

1. `ssh mik0mac@SERVER_IP` — logs in, no password.
2. `ssh root@SERVER_IP` — still works (key).
3. The real test — pretend to be a bot with no key:
   ```
   ssh -o PubkeyAuthentication=no root@SERVER_IP
   ```
   You want `Permission denied (publickey)` — immediately, **without ever
   being asked for a password**. That's the door closing.

Once all three pass, close everything. Done.

## Optional — fail2ban

```
sudo apt install fail2ban
```

Out of the box it watches the SSH log and temporarily bans IPs after repeated
failures. With passwords off it's a second lock on an already-locked door,
but it keeps `auth.log` much quieter.

## Adding another machine's key later

Passwords are off, so a brand-new machine can't use `ssh-copy-id` directly —
it has no key on the server yet and no password fallback. The trick: **use a
machine that already has access to install the new machine's key.** Public
keys are safe to send anywhere (email, AirDrop, chat) — only the private key
(the file without `.pub`) must never leave its machine.

### 1. On the NEW machine — create a key if it doesn't have one

```
ssh-keygen -t ed25519
```

Accept the defaults (Enter through the prompts). This creates
`~/.ssh/id_ed25519` (private — stays put) and `~/.ssh/id_ed25519.pub`
(public — this is what we're shipping).

### 2. Get the public key over to the OLD machine (the one with access)

Any way you like. Display it on the new machine:

```
cat ~/.ssh/id_ed25519.pub
```

It's one line starting with `ssh-ed25519`. Copy it via AirDrop, email,
Messages, a USB stick — doesn't matter, it's public. Save it on the old
machine as e.g. `~/Downloads/newmachine.pub`.

### 3. On the OLD machine — install it on the server

`ssh-copy-id` can install *someone else's* key file, riding on your existing
key for the login:

```
ssh-copy-id -f -i ~/Downloads/newmachine.pub mik0mac@SERVER_IP
```

- `-i ~/Downloads/newmachine.pub` — install *this* key, not your own.
- `-f` — skip the "can I already log in with this key?" check (the old
  machine can't, because it isn't its key — that's the point).

Repeat for root if the new machine should have root access too:

```
ssh-copy-id -f -i ~/Downloads/newmachine.pub root@SERVER_IP
```

### 4. On the NEW machine — verify

```
ssh mik0mac@SERVER_IP
```

Should log straight in, no password.

## If you ever get locked out

Vultr's control panel has a browser-based console (**View Console** on the
instance page) that acts like a physical screen and keyboard plugged into the
server — SSH settings don't apply to it. Log in there and fix or delete
`/etc/ssh/sshd_config.d/00-hardening.conf`, then `systemctl restart ssh`.
