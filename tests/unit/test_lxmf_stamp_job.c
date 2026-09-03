#include <assert.h>
#include <string.h>

#include "reticulum/lxmf.h"

static void complete_with_budget(lxmf_stamp_job_t *job, uint32_t budget) {
    lxmf_stamp_job_progress_t before, after;
    for (size_t polls = 0; polls < 100000u; ++polls) {
        assert(lxmf_stamp_job_progress(job, &before) == LXMF_OK);
        lxmf_status_t status = lxmf_stamp_job_poll(job, budget);
        assert(lxmf_stamp_job_progress(job, &after) == LXMF_OK);
        assert((after.prepared_rounds - before.prepared_rounds) +
                   (after.attempts - before.attempts) <= budget);
        if (status == LXMF_OK) return;
        assert(status == LXMF_ERR_PENDING);
    }
    assert(0 && "deterministic stamp failed to finish within bound");
}

int main(void) {
    uint8_t id[32] = {1}, nonce[32] = {0}, a[32], b[32], value;
    lxmf_stamp_job_t *job = NULL, *second = NULL;
    lxmf_stamp_job_progress_t progress;
    assert(lxmf_stamp_job_create(id, 0, nonce, &job) == LXMF_ERR_ARGUMENT);
    assert(job == NULL);
    assert(lxmf_stamp_job_create(NULL, 1, nonce, &job) == LXMF_ERR_ARGUMENT);
    assert(lxmf_stamp_job_create(id, 1, nonce, NULL) == LXMF_ERR_ARGUMENT);
    assert(lxmf_stamp_job_poll(NULL, 1) == LXMF_ERR_ARGUMENT);
    assert(lxmf_stamp_job_progress(NULL, &progress) == LXMF_ERR_ARGUMENT);
    assert(lxmf_stamp_job_result(NULL, a, NULL) == LXMF_ERR_ARGUMENT);
    assert(lxmf_stamp_job_create_expanded(id, 1, 0, nonce, &job) ==
           LXMF_ERR_ARGUMENT);
    assert(lxmf_stamp_job_create_expanded(id, 1,
        LXMF_STAMP_WORKBLOCK_ROUNDS + 1u, nonce, &job) == LXMF_ERR_ARGUMENT);

    /* Deterministic pinned propagation expansion: Python LXStamper's 1,000
     * rounds finds nonce 0x79 at cost 8 after 122 candidates. */
    assert(lxmf_stamp_job_create_expanded(id, 8,
        LXMF_PROPAGATION_STAMP_WORKBLOCK_ROUNDS, nonce, &job) == LXMF_OK);
    complete_with_budget(job, LXMF_STAMP_POLL_MAX_UNITS);
    assert(lxmf_stamp_job_result(job, a, NULL) == LXMF_OK);
    assert(a[31] == 0x79u);
    for (size_t i = 0; i < 31u; ++i) assert(a[i] == 0u);
    assert(lxmf_stamp_job_progress(job, &progress) == LXMF_OK);
    assert(progress.prepared_rounds ==
           LXMF_PROPAGATION_STAMP_WORKBLOCK_ROUNDS);
    assert(progress.attempts == 122u);
    assert(lxmf_pow_stamp_validate_expanded(id, 8,
        LXMF_PROPAGATION_STAMP_WORKBLOCK_ROUNDS, a, NULL) == LXMF_OK);
    lxmf_stamp_job_destroy(job); job = NULL;

    assert(lxmf_stamp_job_create(id, 8, nonce, &job) == LXMF_OK);
    assert(lxmf_stamp_job_result(job, a, NULL) == LXMF_ERR_PENDING);
    assert(lxmf_stamp_job_poll(job, 0) == LXMF_ERR_PENDING);
    assert(lxmf_stamp_job_poll(job, LXMF_STAMP_POLL_MAX_UNITS + 1) ==
           LXMF_ERR_ARGUMENT);
    assert(lxmf_stamp_job_progress(job, &progress) == LXMF_OK);
    assert(progress.state == LXMF_STAMP_PREPARING);
    assert(progress.prepared_rounds == 0 && progress.attempts == 0);
    complete_with_budget(job, 1);
    assert(lxmf_stamp_job_result(job, a, &value) == LXMF_OK);
    assert(value >= 8);
    assert(lxmf_pow_stamp_validate(id, 8, a, NULL) == LXMF_OK);
    assert(lxmf_stamp_job_progress(job, &progress) == LXMF_OK);
    assert(progress.state == LXMF_STAMP_COMPLETE);
    assert(progress.prepared_rounds == LXMF_STAMP_WORKBLOCK_ROUNDS);
    uint64_t attempts = progress.attempts;

    /* Chunking must not change candidates, workblock, stamp or attempt count. */
    assert(lxmf_stamp_job_create(id, 8, nonce, &second) == LXMF_OK);
    complete_with_budget(second, LXMF_STAMP_POLL_MAX_UNITS);
    assert(lxmf_stamp_job_result(second, b, NULL) == LXMF_OK);
    assert(memcmp(a, b, sizeof a) == 0);
    assert(lxmf_stamp_job_progress(second, &progress) == LXMF_OK);
    assert(progress.attempts == attempts);
    lxmf_stamp_job_cancel(second);
    assert(lxmf_stamp_job_poll(second, 1) == LXMF_OK);
    lxmf_stamp_job_destroy(second);
    lxmf_stamp_job_destroy(job);

    /* Cancellation is immediate both during workblock preparation and search. */
    assert(lxmf_stamp_job_create(id, 255, nonce, &job) == LXMF_OK);
    assert(lxmf_stamp_job_poll(job, 1) == LXMF_ERR_PENDING);
    lxmf_stamp_job_cancel(job);
    assert(lxmf_stamp_job_poll(job, 64) == LXMF_ERR_CANCELLED);
    assert(lxmf_stamp_job_result(job, a, NULL) == LXMF_ERR_CANCELLED);
    assert(lxmf_stamp_job_progress(job, &progress) == LXMF_OK);
    assert(progress.state == LXMF_STAMP_CANCELLED);
    assert(progress.prepared_rounds == 1 && progress.attempts == 0);
    lxmf_stamp_job_destroy(job);

    assert(lxmf_stamp_job_create(id, 255, nonce, &job) == LXMF_OK);
    for (size_t i = 0; i < LXMF_STAMP_WORKBLOCK_ROUNDS; ++i)
        assert(lxmf_stamp_job_poll(job, 1) == LXMF_ERR_PENDING);
    assert(lxmf_stamp_job_progress(job, &progress) == LXMF_OK);
    assert(progress.state == LXMF_STAMP_SEARCHING && progress.attempts == 0);
    assert(lxmf_stamp_job_poll(job, 64) == LXMF_ERR_PENDING);
    lxmf_stamp_job_cancel(job);
    assert(lxmf_stamp_job_poll(job, 64) == LXMF_ERR_CANCELLED);
    assert(lxmf_stamp_job_progress(job, &progress) == LXMF_OK);
    assert(progress.attempts == 64);
    lxmf_stamp_job_destroy(job);

    assert(lxmf_stamp_job_create(id, 1, NULL, &job) == LXMF_OK);
    lxmf_stamp_job_cancel(job);
    lxmf_stamp_job_destroy(job);
    lxmf_stamp_job_cancel(NULL);
    lxmf_stamp_job_destroy(NULL);
    return 0;
}
