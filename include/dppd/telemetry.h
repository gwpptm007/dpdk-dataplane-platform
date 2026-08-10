#ifndef DPPD_TELEMETRY_H
#define DPPD_TELEMETRY_H

struct dppd_runtime;

int dppd_telemetry_register(const struct dppd_runtime *runtime);
void dppd_telemetry_unregister_runtime(void);

#endif
