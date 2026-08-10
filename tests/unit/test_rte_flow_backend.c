#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <string.h>
#include "dppd/rte_flow_backend.h"

struct fake_flow_api_state {
    int validate_calls;
    int create_calls;
    int remove_calls;
    int fail_validate_at;
    int fail_create_at;
    int fail_remove_at;
};

static struct fake_flow_api_state fake;

static void reset_fake(void)
{
    memset(&fake, 0, sizeof(fake));
    fake.fail_validate_at = -1;
    fake.fail_create_at = -1;
    fake.fail_remove_at = -1;
}

static int fake_validate(uint16_t port_id, const struct dppd_rule *rule,
                         struct dppd_flow_error *error)
{
    int call = fake.validate_calls++;

    (void)port_id;
    (void)rule;
    (void)error;
    return call == fake.fail_validate_at ? -ENOTSUP : 0;
}

static int fake_create(uint16_t port_id, const struct dppd_rule *rule,
                       struct dppd_flow_handle *handle,
                       struct dppd_flow_error *error)
{
    int call = fake.create_calls++;

    (void)error;
    memset(handle, 0, sizeof(*handle));
    if (call == fake.fail_create_at)
        return -EIO;
    handle->rule_id = rule->id;
    handle->rule_generation = rule->generation;
    handle->port_id = port_id;
    handle->flow = (struct rte_flow *)(uintptr_t)(rule->id + 1U);
    return 0;
}

static int fake_remove(struct dppd_flow_handle *handle,
                       struct dppd_flow_error *error)
{
    int call = fake.remove_calls++;

    (void)error;
    if (call == fake.fail_remove_at)
        return -EFAULT;
    memset(handle, 0, sizeof(*handle));
    return 0;
}

static struct dppd_rule rule(uint64_t id)
{
    struct dppd_rule result;

    memset(&result, 0, sizeof(result));
    result.id = id;
    result.domain = DPPD_RULE_DOMAIN_INGRESS;
    result.fallback = DPPD_FALLBACK_REQUIRE_HARDWARE;
    result.nb_matches = 1;
    result.matches[0].type = DPPD_MATCH_ETH;
    result.nb_actions = 1;
    result.actions[0].type = DPPD_ACTION_DROP;
    return result;
}

static void items(struct dppd_transaction_item *result, uint32_t count,
                  uint64_t first_id)
{
    uint32_t i;

    memset(result, 0, sizeof(*result) * count);
    for (i = 0; i < count; ++i) {
        result[i].rule = rule(first_id + i);
        result[i].rule.generation = 1;
        result[i].plan.rule_id = first_id + i;
        result[i].plan.rule_generation = 1;
        result[i].plan.backend = DPPD_PLAN_BACKEND_RTE_FLOW;
        result[i].plan.install_port_id = 5;
    }
}

int main(void)
{
    const struct dppd_flow_api api = {
        .validate = fake_validate,
        .create = fake_create,
        .remove = fake_remove,
    };
    struct dppd_rte_flow_backend backend;
    struct dppd_transaction_backends backends;
    struct dppd_transaction_item transaction_items[3];
    struct dppd_transaction transaction;
    struct dppd_flow_error error;

    reset_fake();
    assert(dppd_rte_flow_backend_init(&backend, 4, &api) == 0);
    memset(&backends, 0, sizeof(backends));
    backends.rte_flow = dppd_rte_flow_transaction_backend(&backend);
    items(transaction_items, 2, 100);
    assert(dppd_transaction_init(&transaction, 1, transaction_items, 2) == 0);
    assert(dppd_transaction_run(&transaction, &backends) == 0);
    assert(transaction.state == DPPD_TRANSACTION_COMMITTED);
    assert(fake.validate_calls == 2 && fake.create_calls == 2);
    assert(dppd_rte_flow_backend_count(&backend) == 2);
    assert(dppd_rte_flow_backend_find(&backend, 100) != NULL);

    items(transaction_items, 1, 100);
    transaction_items[0].rule.generation = 2;
    transaction_items[0].plan.rule_generation = 2;
    assert(dppd_transaction_init(&transaction, 2, transaction_items, 1) == 0);
    assert(dppd_transaction_run(&transaction, &backends) == 0);
    assert(dppd_rte_flow_backend_count(&backend) == 3);
    assert(dppd_rte_flow_backend_find(&backend, 100)->rule_generation == 2);
    assert(dppd_rte_flow_backend_find_version(&backend, 100, 1) != NULL);
    assert(dppd_rte_flow_backend_find_version(&backend, 100, 2) != NULL);
    assert(dppd_rte_flow_backend_remove_version(&backend, 100, 1, &error) == 0);
    assert(dppd_rte_flow_backend_remove(&backend, 100, &error) == 0);
    assert(dppd_rte_flow_backend_count(&backend) == 1);
    assert(dppd_rte_flow_backend_fini(&backend) == 0);
    assert(fake.remove_calls == 3);

    reset_fake();
    fake.fail_create_at = 1;
    assert(dppd_rte_flow_backend_init(&backend, 4, &api) == 0);
    backends.rte_flow = dppd_rte_flow_transaction_backend(&backend);
    items(transaction_items, 3, 200);
    assert(dppd_transaction_init(&transaction, 2, transaction_items, 3) == 0);
    assert(dppd_transaction_run(&transaction, &backends) == -EIO);
    assert(transaction.state == DPPD_TRANSACTION_ROLLED_BACK);
    assert(dppd_rte_flow_backend_count(&backend) == 0);
    assert(fake.remove_calls == 1);
    assert(dppd_rte_flow_backend_fini(&backend) == 0);

    reset_fake();
    assert(dppd_rte_flow_backend_init(&backend, 4, &api) == 0);
    backends.rte_flow = dppd_rte_flow_transaction_backend(&backend);
    items(transaction_items, 2, 300);
    transaction_items[1].rule.id = transaction_items[0].rule.id;
    transaction_items[1].plan.rule_id = transaction_items[0].rule.id;
    assert(dppd_transaction_init(&transaction, 3, transaction_items, 2) == 0);
    assert(dppd_transaction_run(&transaction, &backends) == -EEXIST);
    assert(transaction.state == DPPD_TRANSACTION_ROLLED_BACK);
    assert(dppd_rte_flow_backend_count(&backend) == 0);
    assert(fake.create_calls == 0);
    assert(dppd_rte_flow_backend_fini(&backend) == 0);
    return 0;
}
