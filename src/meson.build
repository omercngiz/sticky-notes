gnome_sticky_notes_sources = [
  'main.c',
  'gnome-sticky-notes-application.c',
  'gnome-sticky-notes-autostart.c',
  'gnome-sticky-notes-database.c',
  'gnome-sticky-notes-tray.c',
  'gnome-sticky-notes-window.c',
]

gnome_sticky_notes_deps = [
  dependency('gtk4'),
  dependency('libadwaita-1', version: '>= 1.4'),
  dependency('sqlite3'),
]

gnome_sticky_notes_sources += gnome.compile_resources('gnome-sticky-notes-resources',
  'gnome-sticky-notes.gresource.xml',
  c_name: 'gnome_sticky_notes'
)

executable('gnome-sticky-notes', gnome_sticky_notes_sources,
  dependencies: gnome_sticky_notes_deps,
       install: true,
)
