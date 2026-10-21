# Added by the Sommelier project, 2026-10-22: the cellar tool (LGPL-2.1-or-later).
# A native macOS program: it runs outside Wine, from Sommelier or the service's builder.
cellar_exe = executable('cellar', ['cellar.cpp'],
  include_directories : [ dxmt_include_path, include_directories('../winemetal') ],
  cpp_args            : [ '-ObjC++', '-fobjc-arc' ],
  link_args           : [ '-framework', 'Metal', '-framework', 'Foundation', '-lsqlite3' ],
  native              : true,
)

meson.add_install_script(
  'install.sh', cellar_exe.full_path(), unix_install_dir,
)
