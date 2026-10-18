# 基准复用正式控制面，内存驱动只测控制开销，不作为物理网卡安装速率
executable('benchmark_control_scale', 'control_scale.c', dependencies: dppd_core_dep, install: false)

# 八个队列达到应用深度上限，计时包含队列等待和正式整批账本发布
executable('benchmark_flow_queue', 'flow_queue.c', dependencies: dppd_core_dep, install: false)
