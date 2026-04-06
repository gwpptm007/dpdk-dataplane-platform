app_sources = files('main.c', 'ctrl.c', 'port_init.c', 'worker.c', 'stats.c')
executable('dppd', app_sources,
    include_directories: inc,
    dependencies: dpdk,
    install: false)
