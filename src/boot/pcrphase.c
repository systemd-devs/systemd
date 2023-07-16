/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include <getopt.h>

#include <sd-messages.h>

#include "build.h"
#include "efi-loader.h"
#include "main-func.h"
#include "openssl-util.h"
#include "parse-argument.h"
#include "pcrphase-util.h"
#include "pretty-print.h"
#include "strv.h"
#include "tpm-pcr.h"
#include "tpm2-util.h"

static bool arg_graceful = false;
static char *arg_tpm2_device = NULL;
static char **arg_banks = NULL;
static char *arg_file_system = NULL;
static bool arg_machine_id = false;

STATIC_DESTRUCTOR_REGISTER(arg_banks, strv_freep);
STATIC_DESTRUCTOR_REGISTER(arg_tpm2_device, freep);
STATIC_DESTRUCTOR_REGISTER(arg_file_system, freep);

static int help(int argc, char *argv[], void *userdata) {
        _cleanup_free_ char *link = NULL;
        int r;

        r = terminal_urlify_man("systemd-pcrphase", "8", &link);
        if (r < 0)
                return log_oom();

        printf("%1$s  [OPTIONS...] WORD\n"
               "%1$s  [OPTIONS...] --file-system=PATH\n"
               "%1$s  [OPTIONS...] --machine-id\n"
               "\n%5$sMeasure boot phase into TPM2 PCR 11.%6$s\n"
               "\n%3$sOptions:%4$s\n"
               "  -h --help              Show this help\n"
               "     --version           Print version\n"
               "     --bank=DIGEST       Select TPM bank (SHA1, SHA256)\n"
               "     --tpm2-device=PATH  Use specified TPM2 device\n"
               "     --graceful          Exit gracefully if no TPM2 device is found\n"
               "     --file-system=PATH  Measure UUID/labels of file system into PCR 15\n"
               "     --machine-id        Measure machine ID into PCR 15\n"
               "\nSee the %2$s for details.\n",
               program_invocation_short_name,
               link,
               ansi_underline(),
               ansi_normal(),
               ansi_highlight(),
               ansi_normal());

        return 0;
}

static int parse_argv(int argc, char *argv[]) {
        enum {
                ARG_VERSION = 0x100,
                ARG_BANK,
                ARG_TPM2_DEVICE,
                ARG_GRACEFUL,
                ARG_FILE_SYSTEM,
                ARG_MACHINE_ID,
        };

        static const struct option options[] = {
                { "help",        no_argument,       NULL, 'h'              },
                { "version",     no_argument,       NULL, ARG_VERSION      },
                { "bank",        required_argument, NULL, ARG_BANK         },
                { "tpm2-device", required_argument, NULL, ARG_TPM2_DEVICE  },
                { "graceful",    no_argument,       NULL, ARG_GRACEFUL     },
                { "file-system", required_argument, NULL, ARG_FILE_SYSTEM  },
                { "machine-id",  no_argument,       NULL, ARG_MACHINE_ID   },
                {}
        };

        int c, r;

        assert(argc >= 0);
        assert(argv);

        while ((c = getopt_long(argc, argv, "h", options, NULL)) >= 0)
                switch (c) {

                case 'h':
                        help(0, NULL, NULL);
                        return 0;

                case ARG_VERSION:
                        return version();

                case ARG_BANK: {
                        const EVP_MD *implementation;

                        implementation = EVP_get_digestbyname(optarg);
                        if (!implementation)
                                return log_error_errno(SYNTHETIC_ERRNO(EINVAL), "Unknown bank '%s', refusing.", optarg);

                        if (strv_extend(&arg_banks, EVP_MD_name(implementation)) < 0)
                                return log_oom();

                        break;
                }

                case ARG_TPM2_DEVICE: {
                        _cleanup_free_ char *device = NULL;

                        if (streq(optarg, "list"))
                                return tpm2_list_devices();

                        if (!streq(optarg, "auto")) {
                                device = strdup(optarg);
                                if (!device)
                                        return log_oom();
                        }

                        free_and_replace(arg_tpm2_device, device);
                        break;
                }

                case ARG_GRACEFUL:
                        arg_graceful = true;
                        break;

                case ARG_FILE_SYSTEM:
                        r = parse_path_argument(optarg, /* suppress_root= */ false, &arg_file_system);
                        if (r < 0)
                                return r;

                        break;

                case ARG_MACHINE_ID:
                        arg_machine_id = true;
                        break;

                case '?':
                        return -EINVAL;

                default:
                        assert_not_reached();
                }

        if (arg_file_system && arg_machine_id)
                return log_error_errno(SYNTHETIC_ERRNO(EINVAL), "--file-system= and --machine-id may not be combined.");

        return 1;
}

static int determine_banks(Tpm2Context *c, unsigned target_pcr_nr) {
        _cleanup_strv_free_ char **l = NULL;
        int r;

        assert(c);

        if (!strv_isempty(arg_banks)) /* Explicitly configured? Then use that */
                return 0;

        r = tpm2_get_good_pcr_banks_strv(c, UINT32_C(1) << target_pcr_nr, &l);
        if (r < 0)
                return r;

        strv_free_and_replace(arg_banks, l);
        return 0;
}

static int run(int argc, char *argv[]) {
        _cleanup_free_ char *joined = NULL, *word = NULL;
        Tpm2UserspaceEventType event;
        unsigned target_pcr_nr;
        size_t length;
        int r;

        log_setup();

        r = parse_argv(argc, argv);
        if (r <= 0)
                return r;

        if (arg_file_system) {
                if (optind != argc)
                        return log_error_errno(SYNTHETIC_ERRNO(EINVAL), "Expected no argument.");

                r = pcrphase_file_system_word(arg_file_system, &word, NULL);
                if (r < 0)
                        return r;

                target_pcr_nr = TPM_PCR_INDEX_VOLUME_KEY; /* → PCR 15 */
                event = TPM2_EVENT_FILESYSTEM;

        } else if (arg_machine_id) {

                if (optind != argc)
                        return log_error_errno(SYNTHETIC_ERRNO(EINVAL), "Expected no argument.");

                r = pcrphase_machine_id_word(&word);
                if (r < 0)
                        return r;

                target_pcr_nr = TPM_PCR_INDEX_VOLUME_KEY; /* → PCR 15 */
                event = TPM2_EVENT_MACHINE_ID;

        } else {
                if (optind+1 != argc)
                        return log_error_errno(SYNTHETIC_ERRNO(EINVAL), "Expected a single argument.");

                word = strdup(argv[optind]);
                if (!word)
                        return log_oom();

                /* Refuse to measure an empty word. We want to be able to write the series of measured words
                 * separated by colons, where multiple separating colons are collapsed. Thus it makes sense to
                 * disallow an empty word to avoid ambiguities. */
                if (isempty(word))
                        return log_error_errno(SYNTHETIC_ERRNO(EINVAL), "String to measure cannot be empty, refusing.");

                target_pcr_nr = TPM_PCR_INDEX_KERNEL_IMAGE; /* → PCR 11 */
                event = TPM2_EVENT_PHASE;
        }

        if (arg_graceful && tpm2_support() != TPM2_SUPPORT_FULL) {
                log_notice("No complete TPM2 support detected, exiting gracefully.");
                return EXIT_SUCCESS;
        }

        length = strlen(word);

        /* Skip logic if sd-stub is not used, after all PCR 11 might have a very different purpose then. */
        r = efi_stub_measured(LOG_ERR);
        if (r < 0)
                return r;
        if (r == 0) {
                log_info("Kernel stub did not measure kernel image into PCR %u, skipping userspace measurement, too.", TPM_PCR_INDEX_KERNEL_IMAGE);
                return EXIT_SUCCESS;
        }

        _cleanup_(tpm2_context_unrefp) Tpm2Context *c = NULL;
        r = tpm2_context_new(arg_tpm2_device, &c);
        if (r < 0)
                return r;

        r = determine_banks(c, target_pcr_nr);
        if (r < 0)
                return r;
        if (strv_isempty(arg_banks)) /* Still none? */
                return log_error_errno(SYNTHETIC_ERRNO(ENOENT), "Found a TPM2 without enabled PCR banks. Can't operate.");

        joined = strv_join(arg_banks, ", ");
        if (!joined)
                return log_oom();

        log_debug("Measuring '%s' into PCR index %u, banks %s.", word, target_pcr_nr, joined);

        r = tpm2_extend_bytes(c, arg_banks, target_pcr_nr, word, length, NULL, 0, event, word);
        if (r < 0)
                return r;

        log_struct(LOG_INFO,
                   "MESSAGE_ID=" SD_MESSAGE_TPM_PCR_EXTEND_STR,
                   LOG_MESSAGE("Extended PCR index %u with '%s' (banks %s).", target_pcr_nr, word, joined),
                   "MEASURING=%s", word,
                   "PCR=%u", target_pcr_nr,
                   "BANKS=%s", joined);

        return EXIT_SUCCESS;
}

DEFINE_MAIN_FUNCTION(run);
