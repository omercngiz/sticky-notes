<div align="center">

# 📝 Sticky Notes for GNOME Desktop

**Little notes that live on your desktop.**

Jot things down, make checklists, format your text — and your notes are always
there, even after you restart your computer.

<!-- Tip: add a screenshot here once you have one, e.g.:
![Screenshot](data/screenshots/main.png) -->

</div>

---

## What is this?

Sticky Notes is a small app for the **GNOME desktop on Linux**. Think of
the paper sticky notes you put on your fridge or monitor — but on your screen.

You can:

- ✍️ **Write notes** and keep them open on your desktop.
- ✅ **Make to-do lists** with checkboxes you can tick off.
- 🎨 **Format text** — bold, italic, underline, change the font, size, colour,
  and alignment.
- 🔄 **Get your notes back automatically** every time you log in.
- 💾 **Back up your notes** to a file or email a copy to yourself.

The app quietly runs in the background and shows a small **icon in your system
tray** (top bar), so it's always one click away.

---

## Installing

### The easy way (recommended)

1. Go to the [**Releases**](../../releases) page.
2. Download the latest `.deb` file (for Ubuntu, Debian, Linux Mint, Pop!_OS,
   and similar systems).
3. Double-click the downloaded file and click **Install** — or open a terminal
   in your Downloads folder and run:
   ```sh
   sudo apt install ./sticky-notes_*.deb
   ```
4. Open it from your applications menu — search for **“Sticky Notes”**.

> **Tip:** The app starts automatically when you log in. You can turn this off
> in its Settings.

### Building it yourself (for the curious)

If you'd rather build from the source code, you need a few developer tools
first:

```sh
sudo apt install meson ninja-build build-essential pkg-config gettext \
     libgtk-4-dev libadwaita-1-dev libsqlite3-dev libglib2.0-dev
```

Then build and install it:

```sh
meson setup _build
ninja -C _build
sudo ninja -C _build install
```

Now it'll appear in your applications menu like any other app.

---

## How to use it

### Writing a note

Just click on a note and start typing. Everything you write is **saved
automatically** — there's no Save button to worry about.

To get a new note, click the **menu button** (☰) in the top-left of any note
and choose **New Note**.

### Formatting your text

Click the **format button** (the “A” icon) at the top of a note to open the
formatting tools: **bold, italic, underline, strikethrough**, font and size,
text colour, and alignment.

Handy keyboard shortcuts while typing:

| Shortcut | Does |
| --- | --- |
| `Ctrl` + `B` | **Bold** |
| `Ctrl` + `I` | *Italic* |
| `Ctrl` + `U` | <u>Underline</u> |

### Making a to-do list

Click the **checkbox button** at the top of a note. The line you're on turns
into a to-do item with a checkbox.

- Press **Enter** to start the next to-do item.
- Tick the **checkbox** to mark something done — it gets crossed out.
- Press **Enter** on an empty to-do (or the checkbox button again) to turn it
  back into a normal line.

### Backing up your notes

Open **Settings** (from the ☰ menu) and find **Backup & Restore**:

- **Export** — saves all your notes into a single file you can keep somewhere
  safe.
- **Import** — brings your notes back from a backup file (great for moving to a
  new computer).
- **Email** — opens your email app with a backup attached, so you can send a
  copy to yourself.

---

## Where are my notes stored?

On your own computer, in a small database file at:

```
~/.local/share/gnome-sticky-notes/notes.db
```

Your notes never leave your machine unless *you* export or email them.

---

## Frequently asked

**I closed a note — did I lose it?**
No. Closing a note just hides it. Open the tray icon and choose **Show All
Notes**, or use **New Note** for a fresh one. To permanently remove a note, use
**Delete Note** from its menu.

**The tray icon doesn't show up.**
GNOME doesn't show tray icons by default. Install the
*AppIndicator and KStatusNotifier Support* GNOME Shell extension and the icon
will appear.

**Does it work on Wayland?**
Yes. (One small note: on Wayland an app can't choose where its own windows
appear, so note positions aren't restored — only their sizes are.)

---

## Contributing

Bug reports, ideas, and pull requests are welcome — open an
[issue](../../issues) or a pull request.

## License

Released under the **MIT License** — see [COPYING](COPYING). You're free to use,
modify, and share it.
