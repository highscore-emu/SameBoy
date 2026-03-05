project('sameboy-highscore', ['c', 'cpp'],
  version : '1.0.3',
  default_options: ['c_std=gnu11']
)

version = meson.project_version()
commit = run_command('git', 'describe', '--always', '--dirty', '--match', '', check: false).stdout().strip()

cc = meson.get_compiler('c')

core_dir = get_option('core_install_dir')
if core_dir == ''
  libdir = get_option('prefix') / get_option('libdir')
  core_dir = libdir / 'highscore' / 'cores' / 'sameboy'
endif

rgbasm = find_program('rgbasm')
rgblink = find_program('rgblink')
rgbgfx = find_program('rgbgfx')

add_project_arguments([
  '-fvisibility=hidden',
  '-DGB_DISABLE_TIMEKEEPING',
  '-DGB_DISABLE_CHEAT_SEARCH',
  '-DGB_DISABLE_REWIND',
  '-DGB_DISABLE_DEBUGGER',
  '-DGB_INTERNAL',
  '-DGB_VERSION="@0@"'.format(version),
  '-DGIT_VERSION=" @0@"'.format(commit),
  '-DCORE_DIR="@0@"'.format(core_dir),
  '-Wno-missing-braces',
  '-Wno-multichar',
  '-Wno-unused-result',
], language: 'c')

pb12 = executable('pb12',
  '../BootROMs/pb12.c',
  install: false
)

sameboy_logo_2bpp = custom_target('SameBoyLogo.2bpp',
  input: '../BootROMs/SameBoyLogo.png',
  output: 'SameBoyLogo.2bpp',
  command: [ rgbgfx, '-Z', '-u', '-c', 'embedded', '-o', '@OUTPUT@', '@INPUT@' ]
)

sameboy_logo_pb12 = custom_target('SameBoyLogo.pb12',
  input: sameboy_logo_2bpp,
  output: 'SameBoyLogo.pb12',
  command: pb12,
  feed: true,
  capture: true
)

bootroms_dir = meson.project_source_root() / '..' / 'BootROMs'

sameboy_bootroms_deps = [
  sameboy_logo_pb12,
]

sameboy_boot_o_gen = generator(rgbasm,
  arguments: [ '--include=@0@'.format(bootroms_dir), '--include=.', '-o', '@OUTPUT@', '@INPUT@' ],
  depends: sameboy_bootroms_deps,
  output: '@BASENAME@.o'
)

sameboy_boot_roms = [
  'agb_boot', 'cgb0_boot', 'cgb_boot', 'cgb_boot_fast', 'dmg_boot', 'mgb_boot', 'sgb2_boot', 'sgb_boot',
]

foreach boot_rom : sameboy_boot_roms
  boot_rom_name = '@0@.bin'.format(boot_rom)
  custom_target(boot_rom_name,
    input: sameboy_boot_o_gen.process(bootroms_dir / '@0@.asm'.format(boot_rom)),
    output: boot_rom_name,
    command: [ rgblink, '-x', '-o', '@OUTPUT@', '@INPUT@' ],
    install: true,
    install_dir: core_dir
  )
endforeach

sameboy_deps = [
  dependency('highscore-1'),
  dependency('gio-2.0'),
  cc.find_library('m', required : false),
]

sameboy_include_dirs = [
  include_directories('.'),
  include_directories('../Core'),
]

sameboy_source = [
  '../Core/apu.c',
  '../Core/camera.c',
  '../Core/cheats.c',
  '../Core/display.c',
  '../Core/gb.c',
  '../Core/joypad.c',
  '../Core/mbc.c',
  '../Core/memory.c',
  '../Core/random.c',
  '../Core/rumble.c',
  '../Core/save_state.c',
  '../Core/sgb.c',
  '../Core/sm83_cpu.c',
  '../Core/timing.c',
  '../Core/workboy.c',
  'sameboy-highscore.c',
]

shared_library('sameboy-highscore',
  sameboy_source,
  dependencies : sameboy_deps,
  name_prefix: '',
  include_directories: sameboy_include_dirs,
  install_dir : core_dir,
  install : true,
)

descriptor = configuration_data()
descriptor.set('version', version)
descriptor.set('commit', commit)
configure_file(
  input: 'sameboy.highscore.in',
  output: 'sameboy.highscore',
  configuration: descriptor,
  install_dir: core_dir,
  install: true
)

install_data('sameboy.license',
  install_dir: core_dir
)