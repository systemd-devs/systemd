/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Notes on how to run the fuzzer manually:
 *  1) Build the fuzzers with LLVM's libFuzzer and ASan+UBSan:
 *    $ CC=clang CXX=clang++ meson build-libfuzz -Db_sanitize=address,undefined -Dllvm-fuzz=true -Db_lundef=false
 *
 *  2) Collect some valid inputs:
 *
 * OUT=test/fuzz/fuzz-execute-serialize/initial
 * for section in context command parameters runtime cgroup; do
 *     awk "match(\$0, /startswith\\(.+, \"(exec-${section}-[^\"]+=)\"/, m) { print m[1]; }" \
 *         src/core/execute-serialize.c >>"$OUT"
 *     # Each "section" is delimited by an empty line
 *     echo >>"$OUT"
 * done
 *
 *  3) Run the fuzzer:
 *    $ build-libfuzz/fuzz-execute-serialize test/fuzz/fuzz-execute-serialize
 */

#include <stdio.h>

#include "alloc-util.h"
#include "execute-serialize.h"
#include "fd-util.h"
#include "fuzz.h"
#include "service.h"

static void exec_fuzz_one(FILE *f, FDSet *fdset, bool store_index) {
        _cleanup_(exec_params_deep_clear) ExecParameters params = EXEC_PARAMETERS_INIT(/* flags= */ 0);
        _cleanup_(exec_context_done) ExecContext exec_context = {};
        _cleanup_(exec_command_done) ExecCommand command = {};
        _cleanup_(cgroup_context_done) CGroupContext cgroup_context = {};
        _cleanup_(dynamic_creds_done) DynamicCreds dynamic_creds = {};
        _cleanup_(exec_shared_runtime_done) ExecSharedRuntime shared = {
                .netns_storage_socket = EBADF_PAIR,
                .ipcns_storage_socket = EBADF_PAIR,
        };
        _cleanup_(exec_runtime_clear) ExecRuntime runtime = {
                .ephemeral_storage_socket = EBADF_PAIR,
                .shared = &shared,
                .dynamic_creds = &dynamic_creds,
        };
        int fd_index = 0;

        exec_context_init(&exec_context);
        cgroup_context_init(&cgroup_context);

        (void) exec_deserialize_invocation(f, fdset, NULL, 0, &exec_context, &command, &params, &runtime, &cgroup_context);
        (void) exec_serialize_invocation(f, fdset, store_index ? &fd_index : NULL, &exec_context, &command, &params, &runtime, &cgroup_context);
        (void) exec_deserialize_invocation(f, fdset, NULL, 0, &exec_context, &command, &params, &runtime, &cgroup_context);

        /* Asserts on cleanup */
        params.n_socket_fds = params.n_storage_fds = 0;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
        _cleanup_close_ int devnull = -EBADF;
        _cleanup_fclose_ FILE *f = NULL;
        _cleanup_fdset_free_ FDSet *fdset = NULL;

        fuzz_setup_logging();

        assert_se((devnull = open("/dev/null", O_RDWR | O_CLOEXEC)) >= 0);
        assert_se(fdset = fdset_new());
        assert_se(f = data_to_file(data, size));

        for (size_t i = 0; i < 64; i++)
                fdset_put_dup_indexed(fdset, devnull, i);

        log_info("/* %s - serializing FDs by index */", __func__);
        exec_fuzz_one(f, fdset, /* store_index= */ true);

        fdset = fdset_free(fdset);
        assert_se(fdset = fdset_new());
        for (size_t i = 0; i < 64; i++)
                fdset_put_dup(fdset, devnull);

        log_info("/* %s - serializing FDs by value */", __func__);
        exec_fuzz_one(f, fdset, /* store_index= */ false);

        return 0;
}
