# naughty-sushi

A fork of [GNOME Sushi](https://gitlab.gnome.org/GNOME/sushi), the spacebar previewer,
patched for [Naughty](https://github.com/niccostantini/naughty): it builds against
Papers 50, previews Office files through the OnlyOffice `x2t` converter when
LibreOffice is missing, and opens files from the trash and GVfs locations.

This fork is not affiliated with or endorsed by the GNOME Foundation. The code
keeps its upstream license, GPL-2.0-or-later (see `COPYING`), and the original
copyright notices. OnlyOffice is not included: Sushi only calls an installed copy.
All changes are in the commits on top of upstream.

The original upstream README follows.

---

# Sushi

This is Sushi, a quick previewer for Files (nautilus), the GNOME desktop file manager.

## Using Sushi

Sushi is a DBus-activated service. For Sushi to popup, applications will have to call the `ShowFile` method on the `org.gnome.NautilusPreviewer2` interface.

If you want to debug Sushi without a calling application, you can use the `sushi <filename>` command.

## Reporting issues

Before filing an issue here, confirm that it has not been fixed on the latest nightly build.  The easiest way to do this is by installing the nightly flatpak.

```
flatpak remote-add --if-not-exists gnome-nightly https://nightly.gnome.org/gnome-nightly.flatpakrepo
flatpak install gnome-nightly org.gnome.NautilusPreviewer.Devel
```

After installing the nightly flatpak, you can launch Sushi via the below command, or by initiating a preview from the NautilusDevel nightly flatpak.

`flatpak run org.gnome.NautilusPreviewer.Devel <filename>`

## Plugins

See the [plugins subfolder](plugins/Plugins.md).
