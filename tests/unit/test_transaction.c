#include <assert.h>
#include <errno.h>
#include <string.h>
#include "dppd/transaction.h"

struct fake_backend {
    int validate_calls;
    int prepare_calls;
    int commit_calls;
    int rollback_calls;
    int fail_validate_at;
    int fail_prepare_at;
    int fail_commit_at;
    int fail_rollback_at;
    bool rollback_commit_attempted[4];
};

static int fake_validate(void *context, const struct dppd_transaction_item *item)
{
    struct fake_backend *backend = context;
    int call = backend->validate_calls++;

    (void)item;
    return call == backend->fail_validate_at ? -EINVAL : 0;
}

static int fake_prepare(void *context, const struct dppd_transaction_item *item,
                        uintptr_t *token)
{
    struct fake_backend *backend = context;
    int call = backend->prepare_calls++;

    (void)item;
    if (call == backend->fail_prepare_at)
        return -ENOMEM;
    *token = (uintptr_t)(call + 1);
    return 0;
}

static int fake_commit(void *context, const struct dppd_transaction_item *item,
                       uintptr_t token)
{
    struct fake_backend *backend = context;
    int call = backend->commit_calls++;

    (void)item;
    assert(token != 0);
    return call == backend->fail_commit_at ? -EIO : 0;
}

static int fake_rollback(void *context, const struct dppd_transaction_item *item,
                         uintptr_t token, bool commit_was_attempted)
{
    struct fake_backend *backend = context;
    int call = backend->rollback_calls++;

    (void)item;
    assert(token != 0);
    backend->rollback_commit_attempted[call] = commit_was_attempted;
    return call == backend->fail_rollback_at ? -EFAULT : 0;
}

static struct dppd_transaction_backend operations(struct fake_backend *backend)
{
    struct dppd_transaction_backend result = {
        .context = backend,
        .validate = fake_validate,
        .prepare = fake_prepare,
        .commit = fake_commit,
        .rollback = fake_rollback,
    };
    return result;
}

static void reset_backend(struct fake_backend *backend)
{
    memset(backend, 0, sizeof(*backend));
    backend->fail_validate_at = -1;
    backend->fail_prepare_at = -1;
    backend->fail_commit_at = -1;
    backend->fail_rollback_at = -1;
}

static void reset_items(struct dppd_transaction_item *items, uint32_t count)
{
    uint32_t i;

    memset(items, 0, sizeof(*items) * count);
    for (i = 0; i < count; ++i) {
        items[i].rule.id = i + 1U;
        items[i].rule.generation = 1;
        items[i].plan.rule_id = items[i].rule.id;
        items[i].plan.rule_generation = items[i].rule.generation;
        items[i].plan.backend = DPPD_PLAN_BACKEND_RTE_FLOW;
    }
}

int main(void)
{
    struct fake_backend fake;
    struct dppd_transaction_backends backends;
    struct dppd_transaction_item items[3];
    struct dppd_transaction transaction;

    reset_backend(&fake);
    backends.rte_flow = operations(&fake);
    backends.software = operations(&fake);
    reset_items(items, 3);
    assert(dppd_transaction_init(&transaction, 1, items, 3) == 0);
    assert(dppd_transaction_run(&transaction, &backends) == 0);
    assert(transaction.state == DPPD_TRANSACTION_COMMITTED);
    assert(fake.validate_calls == 3 && fake.prepare_calls == 3);
    assert(fake.commit_calls == 3 && fake.rollback_calls == 0);

    reset_backend(&fake);
    fake.fail_validate_at = 1;
    reset_items(items, 3);
    assert(dppd_transaction_init(&transaction, 2, items, 3) == 0);
    assert(dppd_transaction_run(&transaction, &backends) == -EINVAL);
    assert(transaction.state == DPPD_TRANSACTION_FAILED);
    assert(fake.prepare_calls == 0 && fake.rollback_calls == 0);

    reset_backend(&fake);
    fake.fail_prepare_at = 1;
    reset_items(items, 3);
    assert(dppd_transaction_init(&transaction, 3, items, 3) == 0);
    assert(dppd_transaction_run(&transaction, &backends) == -ENOMEM);
    assert(transaction.state == DPPD_TRANSACTION_ROLLED_BACK);
    assert(fake.commit_calls == 0 && fake.rollback_calls == 1);
    assert(!fake.rollback_commit_attempted[0]);

    reset_backend(&fake);
    fake.fail_commit_at = 1;
    reset_items(items, 3);
    assert(dppd_transaction_init(&transaction, 4, items, 3) == 0);
    assert(dppd_transaction_run(&transaction, &backends) == -EIO);
    assert(transaction.state == DPPD_TRANSACTION_ROLLED_BACK);
    assert(fake.rollback_calls == 3);
    assert(!fake.rollback_commit_attempted[0]);
    assert(fake.rollback_commit_attempted[1]);
    assert(fake.rollback_commit_attempted[2]);

    reset_backend(&fake);
    fake.fail_commit_at = 0;
    fake.fail_rollback_at = 0;
    reset_items(items, 3);
    assert(dppd_transaction_init(&transaction, 5, items, 3) == 0);
    assert(dppd_transaction_run(&transaction, &backends) == -EIO);
    assert(transaction.state == DPPD_TRANSACTION_FAILED);
    assert(transaction.failure_code == -EIO);
    assert(transaction.rollback_code == -EFAULT);
    return 0;
}
