# 配置边界

当前版本只接受启动 CLI，不读取 ACL/NAT/route 文本文件。旧配置文件没有接入执行路径，已删除，避免产生“配置已生效”的错觉。

后续 northbound schema 应直接表达 versioned intent、endpoint selector、rule IR、fallback policy 和 transaction id，并经过 schema 校验与能力规划后才能发布。
