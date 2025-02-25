/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include <getopt.h>

#include "ansi-color.h"
#include "authenticode.h"
#include "build.h"
#include "copy.h"
#include "efi-fundamental.h"
#include "env-util.h"
#include "fd-util.h"
#include "fileio.h"
#include "io-util.h"
#include "log.h"
#include "main-func.h"
#include "openssl-util.h"
#include "parse-argument.h"
#include "pe-binary.h"
#include "pretty-print.h"
#include "stat-util.h"
#include "tmpfile-util.h"
#include "verbs.h"

static char *arg_output = NULL;
static char *arg_certificate = NULL;
static CertificateSourceType arg_certificate_source_type = OPENSSL_CERTIFICATE_SOURCE_FILE;
static char *arg_certificate_source = NULL;
static char *arg_private_key = NULL;
static KeySourceType arg_private_key_source_type = OPENSSL_KEY_SOURCE_FILE;
static char *arg_private_key_source = NULL;
static STACK_OF(X509_ATTRIBUTE) *arg_signed_attributes = NULL;
static struct iovec arg_signed_attributes_signature = {};

STATIC_DESTRUCTOR_REGISTER(arg_output, freep);
STATIC_DESTRUCTOR_REGISTER(arg_certificate, freep);
STATIC_DESTRUCTOR_REGISTER(arg_certificate_source, freep);
STATIC_DESTRUCTOR_REGISTER(arg_private_key, freep);
STATIC_DESTRUCTOR_REGISTER(arg_private_key_source, freep);
STATIC_DESTRUCTOR_REGISTER(arg_signed_attributes, x509_attribute_free_manyp);
STATIC_DESTRUCTOR_REGISTER(arg_signed_attributes_signature, iovec_done);

static int help(int argc, char *argv[], void *userdata) {
        _cleanup_free_ char *link = NULL;
        int r;

        r = terminal_urlify_man("systemd-sbsign", "1", &link);
        if (r < 0)
                return log_oom();

        printf("%1$s  [OPTIONS...] COMMAND ...\n"
               "\n%5$sSign binaries for EFI Secure Boot%6$s\n"
               "\n%3$sCommands:%4$s\n"
               "  sign EXEFILE           Sign the given binary for EFI Secure Boot\n"
               "\n%3$sOptions:%4$s\n"
               "  -h --help              Show this help\n"
               "     --version           Print version\n"
               "     --output            Where to write the signed PE binary\n"
               "     --certificate=PATH|URI\n"
               "                         PEM certificate to use for signing, or a provider\n"
               "                         specific designation if --certificate-source= is used\n"
               "     --certificate-source=file|provider:PROVIDER\n"
               "                         Specify how to interpret the certificate from\n"
               "                         --certificate=. Allows the certificate to be loaded\n"
               "                         from an OpenSSL provider\n"
               "     --private-key=KEY   Private key (PEM) to sign with\n"
               "     --private-key-source=file|provider:PROVIDER|engine:ENGINE\n"
               "                         Specify how to use KEY for --private-key=. Allows\n"
               "                         an OpenSSL engine/provider to be used for signing\n"
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
                ARG_OUTPUT,
                ARG_CERTIFICATE,
                ARG_CERTIFICATE_SOURCE,
                ARG_PRIVATE_KEY,
                ARG_PRIVATE_KEY_SOURCE,
                ARG_SIGNED_ATTRIBUTES,
                ARG_SIGNED_ATTRIBUTES_SIGNATURE,
        };

        static const struct option options[] = {
                { "help",                        no_argument,       NULL, 'h'                             },
                { "version",                     no_argument,       NULL, ARG_VERSION                     },
                { "output",                      required_argument, NULL, ARG_OUTPUT                      },
                { "certificate",                 required_argument, NULL, ARG_CERTIFICATE                 },
                { "certificate-source",          required_argument, NULL, ARG_CERTIFICATE_SOURCE          },
                { "private-key",                 required_argument, NULL, ARG_PRIVATE_KEY                 },
                { "private-key-source",          required_argument, NULL, ARG_PRIVATE_KEY_SOURCE          },
                { "signed-attributes",           required_argument, NULL, ARG_SIGNED_ATTRIBUTES           },
                { "signed-attributes-signature", required_argument, NULL, ARG_SIGNED_ATTRIBUTES_SIGNATURE },
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

                case ARG_OUTPUT:
                        r = parse_path_argument(optarg, /*suppress_root=*/ false, &arg_output);
                        if (r < 0)
                                return r;

                        break;

                case ARG_CERTIFICATE:
                        r = free_and_strdup_warn(&arg_certificate, optarg);
                        if (r < 0)
                                return r;
                        break;

                case ARG_CERTIFICATE_SOURCE:
                        r = parse_openssl_certificate_source_argument(
                                        optarg,
                                        &arg_certificate_source,
                                        &arg_certificate_source_type);
                        if (r < 0)
                                return r;
                        break;

                case ARG_PRIVATE_KEY:
                        r = free_and_strdup_warn(&arg_private_key, optarg);
                        if (r < 0)
                                return r;

                        break;

                case ARG_PRIVATE_KEY_SOURCE:
                        r = parse_openssl_key_source_argument(
                                        optarg,
                                        &arg_private_key_source,
                                        &arg_private_key_source_type);
                        if (r < 0)
                                return r;

                        break;

                case ARG_SIGNED_ATTRIBUTES: {
                        _cleanup_free_ void *content = NULL;
                        size_t contentsz;

                        r = read_full_file(optarg, (char**) &content, &contentsz);
                        if (r < 0)
                                return log_error_errno(r, "Failed to read signed attributes file '%s': %m", optarg);

                        const uint8_t *p = content;
                        if (!ASN1_item_d2i((ASN1_VALUE **) &arg_signed_attributes, &p, contentsz, ASN1_ITEM_rptr(PKCS7_ATTR_SIGN)))
                                return log_error_errno(r, "Failed to parse signed attributes: %s",
                                                       ERR_error_string(ERR_get_error(), NULL));

                        break;
                }

                case ARG_SIGNED_ATTRIBUTES_SIGNATURE: {
                        _cleanup_free_ void *content = NULL;
                        size_t contentsz;

                        r = read_full_file(optarg, (char**) &content, &contentsz);
                        if (r < 0)
                                return log_error_errno(r, "Failed to read signed attributes signature file '%s': %m", optarg);

                        arg_signed_attributes_signature = IOVEC_MAKE(TAKE_PTR(content), contentsz);
                        break;
                }

                case '?':
                        return -EINVAL;

                default:
                        assert_not_reached();
                }

        if (arg_private_key_source && !arg_certificate)
                return log_error_errno(SYNTHETIC_ERRNO(EINVAL), "When using --private-key-source=, --certificate= must be specified.");

        if (!!arg_signed_attributes != iovec_is_set(&arg_signed_attributes_signature))
                return log_error_errno(SYNTHETIC_ERRNO(EINVAL), "--signed-attributes= and --signed-attributes-signature= must always be used together.");

        return 1;
}

static int spc_indirect_data_configure(const void *digest, size_t digestsz, uint8_t **ret_idc, size_t *ret_idcsz) {
        assert(digest);
        assert(ret_idc);
        assert(ret_idcsz);

        /* <<<Obsolete>>> in unicode bytes. */
        static const uint8_t obsolete[] = {
                0x00, 0x3c, 0x00, 0x3c, 0x00, 0x3c, 0x00, 0x4f,
                0x00, 0x62, 0x00, 0x73, 0x00, 0x6f, 0x00, 0x6c,
                0x00, 0x65, 0x00, 0x74, 0x00, 0x65, 0x00, 0x3e,
                0x00, 0x3e, 0x00, 0x3e
        };

        _cleanup_(SpcLink_freep) SpcLink *link = SpcLink_new();
        if (!link)
                return log_oom();

        link->type = 2;
        link->value.file = SpcString_new();
        if (!link->value.file)
                return log_oom();

        link->value.file->type = 0;
        link->value.file->value.unicode = ASN1_BMPSTRING_new();
        if (!link->value.file->value.unicode)
                return log_oom();

        if (ASN1_STRING_set(link->value.file->value.unicode, obsolete, sizeof(obsolete)) == 0)
                return log_error_errno(SYNTHETIC_ERRNO(EIO), "Failed to set ASN1 string: %s",
                                       ERR_error_string(ERR_get_error(), NULL));

        _cleanup_(SpcPeImageData_freep) SpcPeImageData *peid = SpcPeImageData_new();
        if (!peid)
                return log_oom();

        peid->file = TAKE_PTR(link);

        _cleanup_free_ uint8_t *peidraw = NULL;
        int peidrawsz = i2d_SpcPeImageData(peid, &peidraw);
        if (peidrawsz < 0)
                return log_error_errno(SYNTHETIC_ERRNO(EIO), "Failed to convert SpcPeImageData to BER: %s",
                                       ERR_error_string(ERR_get_error(), NULL));

        _cleanup_(SpcIndirectDataContent_freep) SpcIndirectDataContent *idc = SpcIndirectDataContent_new();
        idc->data->value = ASN1_TYPE_new();
        if (!idc->data->value)
                return log_oom();

        idc->data->value->type = V_ASN1_SEQUENCE;
        idc->data->value->value.sequence = ASN1_STRING_new();
        if (!idc->data->value->value.sequence)
                return log_oom();

        idc->data->type = OBJ_txt2obj(SPC_PE_IMAGE_DATA_OBJID, /*no_name=*/ 1);
        if (!idc->data->type)
                return log_error_errno(SYNTHETIC_ERRNO(EIO), "Failed to get SpcPeImageData object: %s",
                                       ERR_error_string(ERR_get_error(), NULL));

        idc->data->value->value.sequence->data = TAKE_PTR(peidraw);
        idc->data->value->value.sequence->length = peidrawsz;
        idc->messageDigest->digestAlgorithm->algorithm = OBJ_nid2obj(NID_sha256);
        if (!idc->messageDigest->digestAlgorithm->algorithm)
                return log_error_errno(SYNTHETIC_ERRNO(EIO), "Failed to get SHA256 object: %s",
                                       ERR_error_string(ERR_get_error(), NULL));

        idc->messageDigest->digestAlgorithm->parameters = ASN1_TYPE_new();
        if (!idc->messageDigest->digestAlgorithm->parameters)
                return log_oom();

        idc->messageDigest->digestAlgorithm->parameters->type = V_ASN1_NULL;

        if (ASN1_OCTET_STRING_set(idc->messageDigest->digest, digest, digestsz) == 0)
                return log_error_errno(SYNTHETIC_ERRNO(EIO), "Failed to set digest: %s",
                                       ERR_error_string(ERR_get_error(), NULL));

        _cleanup_free_ uint8_t *idcraw = NULL;
        int idcrawsz = i2d_SpcIndirectDataContent(idc, &idcraw);
        if (idcrawsz < 0)
                return log_error_errno(SYNTHETIC_ERRNO(EIO), "Failed to convert SpcIndirectDataContent to BER: %s",
                                       ERR_error_string(ERR_get_error(), NULL));

        *ret_idc = TAKE_PTR(idcraw);
        *ret_idcsz = (size_t) idcrawsz;

        return 0;
}

static int asn1_timestamp(ASN1_TIME **ret) {
        ASN1_TIME *time;
        uint64_t epoch = UINT64_MAX;
        int r;

        assert(ret);

        r = secure_getenv_uint64("SOURCE_DATE_EPOCH", &epoch);
        if (r != -ENXIO)
                log_debug_errno(r, "Failed to parse $SOURCE_DATE_EPOCH, ignoring: %m");

        if (epoch == UINT64_MAX) {
                time = X509_gmtime_adj(NULL, 0);
                if (!time)
                        return log_error_errno(SYNTHETIC_ERRNO(EIO), "Failed to get current time: %s",
                                               ERR_error_string(ERR_get_error(), NULL));
        } else {
                time = ASN1_TIME_set(NULL, (time_t) (epoch / USEC_PER_SEC));
                if (!time)
                        return log_oom();
        }

        *ret = TAKE_PTR(time);

        return 0;
}

static int pkcs7_configure(
                X509 *certificate,
                EVP_PKEY *private_key,
                STACK_OF(X509_ATTRIBUTE) *signed_attributes,
                PKCS7 **ret_p7,
                PKCS7_SIGNER_INFO **ret_si) {
        int r;

        assert(certificate);
        assert(ret_p7);
        assert(ret_si);

        _cleanup_(PKCS7_freep) PKCS7 *p7 = NULL;
        PKCS7_SIGNER_INFO *si;
        r = pkcs7_new(certificate, private_key, &p7, &si);
        if (r < 0)
                return log_error_errno(r, "Failed to allocate PKCS# context: %m");

        if (signed_attributes) {
                si->auth_attr = signed_attributes;

                *ret_p7 = TAKE_PTR(p7);
                *ret_si = TAKE_PTR(si);
                return 0;
        }

        _cleanup_(x509_algor_free_manyp) STACK_OF(X509_ALGOR) *smcap = sk_X509_ALGOR_new_null();
        if (!smcap)
                return log_oom();

        if (PKCS7_add_attrib_smimecap(si, smcap) == 0)
                return log_error_errno(SYNTHETIC_ERRNO(EIO), "Failed to add smimecap signed attribute to signer info: %s",
                                       ERR_error_string(ERR_get_error(), NULL));

        if (PKCS7_add_attrib_content_type(si, NULL) == 0)
                return log_error_errno(SYNTHETIC_ERRNO(EIO), "Failed to add content type signed attribute to signer info: %s",
                                       ERR_error_string(ERR_get_error(), NULL));

        _cleanup_(ASN1_TIME_freep) ASN1_TIME *time = NULL;
        r = asn1_timestamp(&time);
        if (r < 0)
                return r;

        if (PKCS7_add0_attrib_signing_time(si, time) == 0)
                return log_error_errno(SYNTHETIC_ERRNO(EIO), "Failed to add signing time signed attribute to signer info: %s",
                                       ERR_error_string(ERR_get_error(), NULL));

        TAKE_PTR(time);

        ASN1_OBJECT *idc = OBJ_txt2obj(SPC_INDIRECT_DATA_OBJID, /* no_name= */ true);
        if (!idc)
                return log_error_errno(SYNTHETIC_ERRNO(EIO), "Failed to get SpcIndirectDataContent object: %s",
                                       ERR_error_string(ERR_get_error(), NULL));

        if (PKCS7_add_signed_attribute(si, NID_pkcs9_contentType, V_ASN1_OBJECT, idc) == 0)
                return log_error_errno(SYNTHETIC_ERRNO(EIO), "Failed to add signed attribute to pkcs7 signer info: %s",
                                       ERR_error_string(ERR_get_error(), NULL));

        *ret_p7 = TAKE_PTR(p7);
        *ret_si = TAKE_PTR(si);
        return 0;
}

static int pkcs7_data_configure(PKCS7* p7, const void *data, size_t size, BIO **ret) {
        assert(ret);

        _cleanup_(BIO_free_allp) BIO *bio = PKCS7_dataInit(p7, NULL);
        if (!bio)
                return log_error_errno(SYNTHETIC_ERRNO(EIO), "Failed to create PKCS7 data bio: %s",
                                       ERR_error_string(ERR_get_error(), NULL));

        int tag, class;
        long psz;
        const uint8_t *p = data;

        /* This function weirdly enough reports errors by setting the 0x80 bit in its return value. */
        if (ASN1_get_object(&p, &psz, &tag, &class, size) & 0x80)
                return log_error_errno(SYNTHETIC_ERRNO(EIO), "Failed to parse ASN.1 object: %s",
                                       ERR_error_string(ERR_get_error(), NULL));

        if (BIO_write(bio, p, psz) < 0)
                return log_error_errno(SYNTHETIC_ERRNO(EIO), "Failed to write to PKCS7 data bio: %s",
                                       ERR_error_string(ERR_get_error(), NULL));

        *ret = TAKE_PTR(bio);

        return 0;
}


static int verb_sign(int argc, char *argv[], void *userdata) {
        _cleanup_(openssl_ask_password_ui_freep) OpenSSLAskPasswordUI *ui = NULL;
        _cleanup_(EVP_PKEY_freep) EVP_PKEY *private_key = NULL;
        _cleanup_(X509_freep) X509 *certificate = NULL;
        int r;

        if (argc < 2)
                return log_error_errno(SYNTHETIC_ERRNO(EINVAL), "No input file specified");

        if (!arg_certificate)
                return log_error_errno(SYNTHETIC_ERRNO(EINVAL),
                                       "No certificate specified, use --certificate=");

        if (!arg_private_key && !iovec_is_set(&arg_signed_attributes_signature))
                return log_error_errno(SYNTHETIC_ERRNO(EINVAL),
                                       "No private key or signed attributes signature specified, use --private-key= or --signed-attributes-signature=.");

        if (!arg_output)
                return log_error_errno(SYNTHETIC_ERRNO(EINVAL), "No output specified, use --output=");

        if (arg_certificate_source_type == OPENSSL_CERTIFICATE_SOURCE_FILE) {
                r = parse_path_argument(arg_certificate, /*suppress_root=*/ false, &arg_certificate);
                if (r < 0)
                        return r;
        }

        r = openssl_load_x509_certificate(
                        arg_certificate_source_type,
                        arg_certificate_source,
                        arg_certificate,
                        &certificate);
        if (r < 0)
                return log_error_errno(r, "Failed to load X.509 certificate from %s: %m", arg_certificate);

        if (arg_private_key) {
                if (arg_private_key_source_type == OPENSSL_KEY_SOURCE_FILE) {
                        r = parse_path_argument(arg_private_key, /* suppress_root= */ false, &arg_private_key);
                        if (r < 0)
                                return log_error_errno(r, "Failed to parse private key path %s: %m", arg_private_key);
                }

                r = openssl_load_private_key(
                                arg_private_key_source_type,
                                arg_private_key_source,
                                arg_private_key,
                                &(AskPasswordRequest) {
                                        .tty_fd = -EBADF,
                                        .id = "sbsign-private-key-pin",
                                        .keyring = arg_private_key,
                                        .credential = "sbsign.private-key-pin",
                                        .until = USEC_INFINITY,
                                        .hup_fd = -EBADF,
                                },
                                &private_key,
                                &ui);
                if (r < 0)
                        return log_error_errno(r, "Failed to load private key from %s: %m", arg_private_key);
        }

        _cleanup_close_ int srcfd = open(argv[1], O_RDONLY|O_CLOEXEC);
        if (srcfd < 0)
                return log_error_errno(errno, "Failed to open %s: %m", argv[1]);

        struct stat st;
        if (fstat(srcfd, &st) < 0)
                return log_error_errno(errno, "Failed to stat %s: %m", argv[1]);

        r = stat_verify_regular(&st);
        if (r < 0)
                return log_error_errno(r, "%s is not a regular file: %m", argv[1]);

        _cleanup_(unlink_and_freep) char *tmp = NULL;
        _cleanup_close_ int dstfd = open_tmpfile_linkable(arg_output, O_RDWR|O_CLOEXEC, &tmp);
        if (dstfd < 0)
                return log_error_errno(r, "Failed to open temporary file: %m");

        r = fchmod_umask(dstfd, 0666);
        if (r < 0)
                log_debug_errno(r, "Failed to change temporary file mode: %m");

        r = copy_bytes(srcfd, dstfd, UINT64_MAX, COPY_REFLINK);
        if (r < 0)
                return log_error_errno(r, "Failed to copy %s to %s: %m", argv[1], tmp);

        _cleanup_free_ void *pehash = NULL;
        size_t pehashsz;
        r = pe_hash(dstfd, EVP_sha256(), &pehash, &pehashsz);
        if (r < 0)
                return log_error_errno(r, "Failed to hash PE binary %s: %m", argv[0]);

        _cleanup_free_ uint8_t *idcraw = NULL;
        size_t idcrawsz;
        r = spc_indirect_data_configure(pehash, pehashsz, &idcraw, &idcrawsz);
        if (r < 0)
                return r;

        _cleanup_(PKCS7_freep) PKCS7 *p7 = NULL;
        PKCS7_SIGNER_INFO *si;
        r = pkcs7_configure(certificate, private_key, arg_signed_attributes, &p7, &si);
        if (r < 0)
                return r;

        TAKE_PTR(arg_signed_attributes);

        if (iovec_is_set(&arg_signed_attributes_signature)) {
                if (ASN1_STRING_set(si->enc_digest, arg_signed_attributes_signature.iov_base, arg_signed_attributes_signature.iov_len) == 0)
                        return log_error_errno(SYNTHETIC_ERRNO(EIO), "Failed to set signer info signed digest: %s",
                                               ERR_error_string(ERR_get_error(), NULL));
        } else {
                _cleanup_(BIO_free_allp) BIO *bio = NULL;
                r = pkcs7_data_configure(p7, idcraw, idcrawsz, &bio);
                if (r < 0)
                        return r;

                if (PKCS7_dataFinal(p7, bio) == 0)
                        return log_error_errno(SYNTHETIC_ERRNO(EIO), "Failed to sign data: %s",
                                               ERR_error_string(ERR_get_error(), NULL));
        }

        _cleanup_(PKCS7_freep) PKCS7 *p7c = PKCS7_new();
        if (!p7c)
                return log_oom();

        p7c->type = OBJ_txt2obj(SPC_INDIRECT_DATA_OBJID, /* no_name= */ true);
        if (!p7c->type)
                return log_error_errno(SYNTHETIC_ERRNO(EIO), "Failed to get SpcIndirectDataContent object: %s",
                                       ERR_error_string(ERR_get_error(), NULL));

        p7c->d.other = ASN1_TYPE_new();
        if (!p7c->d.other)
                return log_oom();

        p7c->d.other->type = V_ASN1_SEQUENCE;
        p7c->d.other->value.sequence = ASN1_STRING_new();
        if (!p7c->d.other->value.sequence)
                return log_oom();

        if (ASN1_STRING_set(p7c->d.other->value.sequence, idcraw, idcrawsz) == 0)
                return log_error_errno(SYNTHETIC_ERRNO(EIO), "Failed to set ASN1 string: %s",
                                       ERR_error_string(ERR_get_error(), NULL));

        if (PKCS7_set_content(p7, p7c) == 0)
                return log_error_errno(SYNTHETIC_ERRNO(EIO), "Failed to set PKCS7 data: %s",
                                       ERR_error_string(ERR_get_error(), NULL));

        TAKE_PTR(p7c);

        _cleanup_free_ uint8_t *sig = NULL;
        int sigsz = i2d_PKCS7(p7, &sig);
        if (sigsz < 0)
                return log_error_errno(SYNTHETIC_ERRNO(EIO), "Failed to convert PKCS7 signature to DER: %s",
                                       ERR_error_string(ERR_get_error(), NULL));

        _cleanup_free_ IMAGE_DOS_HEADER *dos_header = NULL;
        _cleanup_free_ PeHeader *pe_header = NULL;
        r = pe_load_headers(srcfd, &dos_header, &pe_header);
        if (r < 0)
                return log_error_errno(r, "Failed to load headers from PE file: %m");

        const IMAGE_DATA_DIRECTORY *certificate_table;
        certificate_table = pe_header_get_data_directory(pe_header, IMAGE_DATA_DIRECTORY_INDEX_CERTIFICATION_TABLE);
        if (!certificate_table)
                return log_error_errno(SYNTHETIC_ERRNO(EBADMSG), "File lacks certificate table.");

        off_t end = st.st_size;
        ssize_t n;

        if (st.st_size % 8 != 0) {
                if (certificate_table->VirtualAddress != 0)
                        return log_error_errno(SYNTHETIC_ERRNO(EBADMSG), "Certificate table is not aligned to 8 bytes");

                n = pwrite(dstfd, (const uint8_t[8]) {}, 8 - (st.st_size % 8), st.st_size);
                if (n < 0)
                        return log_error_errno(errno, "Failed to write zero padding: %m");
                if (n != 8 - (st.st_size % 8))
                        return log_error_errno(SYNTHETIC_ERRNO(EIO), "Short write while writing zero padding.");

                end += n;
        }

        uint32_t certsz = offsetof(WIN_CERTIFICATE, bCertificate) + sigsz;
        n = pwrite(dstfd,
                   &(WIN_CERTIFICATE) {
                           .wRevision = htole16(0x200),
                           .wCertificateType = htole16(0x0002), /* PKCS7 signedData */
                           .dwLength = htole32(ROUND_UP(certsz, 8)),
                   },
                   sizeof(WIN_CERTIFICATE),
                   end);
        if (n < 0)
                return log_error_errno(errno, "Failed to write certificate header: %m");
        if (n != sizeof(WIN_CERTIFICATE))
                return log_error_errno(SYNTHETIC_ERRNO(EIO), "Short write while writing certificate header.");

        end += n;

        n = pwrite(dstfd, sig, sigsz, end);
        if (n < 0)
                return log_error_errno(errno, "Failed to write signature: %m");
        if (n != sigsz)
                return log_error_errno(SYNTHETIC_ERRNO(EIO), "Short write while writing signature.");

        end += n;

        if (certsz % 8 != 0) {
                n = pwrite(dstfd, (const uint8_t[8]) {}, 8 - (certsz % 8), end);
                if (n < 0)
                        return log_error_errno(errno, "Failed to write zero padding: %m");
                if ((size_t) n != 8 - (certsz % 8))
                        return log_error_errno(SYNTHETIC_ERRNO(EIO), "Short write while writing zero padding.");
        }

        n = pwrite(dstfd,
                   &(IMAGE_DATA_DIRECTORY) {
                           .VirtualAddress = certificate_table->VirtualAddress ?: htole32(ROUND_UP(st.st_size, 8)),
                           .Size = htole32(le32toh(certificate_table->Size) + ROUND_UP(certsz, 8)),
                   },
                   sizeof(IMAGE_DATA_DIRECTORY),
                   le32toh(dos_header->e_lfanew) + PE_HEADER_OPTIONAL_FIELD_OFFSET(pe_header, DataDirectory[IMAGE_DATA_DIRECTORY_INDEX_CERTIFICATION_TABLE]));
        if (n < 0)
                return log_error_errno(errno, "Failed to update PE certificate table: %m");
        if ((size_t) n != sizeof(IMAGE_DATA_DIRECTORY))
                return log_error_errno(SYNTHETIC_ERRNO(EIO), "Short write while updating PE certificate table.");

        uint32_t checksum;
        r = pe_checksum(dstfd, &checksum);
        if (r < 0)
                return log_error_errno(r, "Failed to calculate PE file checksum: %m");

        n = pwrite(dstfd,
                   &(le32_t) { htole32(checksum) },
                   sizeof(le32_t),
                   le32toh(dos_header->e_lfanew) + offsetof(PeHeader, optional.CheckSum));
        if (n < 0)
                return log_error_errno(errno, "Failed to update PE checksum: %m");
        if ((size_t) n != sizeof(le32_t))
                return log_error_errno(SYNTHETIC_ERRNO(EIO), "Short write while updating PE checksum.");

        r = link_tmpfile(dstfd, tmp, arg_output, LINK_TMPFILE_REPLACE|LINK_TMPFILE_SYNC);
        if (r < 0)
                return log_error_errno(r, "Failed to link temporary file to %s: %m", arg_output);

        log_info("Wrote signed PE binary to %s", arg_output);
        return 0;
}

static int pkcs7_digest_attribute_configure(PKCS7 *p7, BIO *data, PKCS7_SIGNER_INFO *si) {
        assert(p7);
        assert(data);
        assert(si);

        BIO *mdbio = BIO_find_type(data, BIO_TYPE_MD);
        if (mdbio == NULL)
                return log_error_errno(SYNTHETIC_ERRNO(EIO), "Failed to find digest bio: %s",
                                       ERR_error_string(ERR_get_error(), NULL));

        EVP_MD_CTX *mdc;
        BIO_get_md_ctx(mdbio, &mdc);
        if (mdc == NULL)
                return log_error_errno(SYNTHETIC_ERRNO(EIO), "Failed to get digest context from bio: %s",
                                       ERR_error_string(ERR_get_error(), NULL));

        unsigned char digest[EVP_MAX_MD_SIZE];
        unsigned digestsz;

        if (EVP_DigestFinal_ex(mdc, digest, &digestsz) == 0)
                return log_error_errno(SYNTHETIC_ERRNO(EIO), "Failed to get digest: %s",
                                       ERR_error_string(ERR_get_error(), NULL));

        if (PKCS7_add1_attrib_digest(si, digest, digestsz) == 0)
                return log_error_errno(SYNTHETIC_ERRNO(EIO), "Failed to add PKCS9 message digest signed attribute to signer info: %s",
                                       ERR_error_string(ERR_get_error(), NULL));

        return 0;
}

static int verb_export_signed_attributes(int argc, char *argv[], void *userdata) {
        _cleanup_(X509_freep) X509 *certificate = NULL;
        int r;

        if (argc < 2)
                return log_error_errno(SYNTHETIC_ERRNO(EINVAL), "No input file specified");

        if (!arg_certificate)
                return log_error_errno(SYNTHETIC_ERRNO(EINVAL),
                                "No certificate specified, use --certificate=");

        if (!arg_output)
                return log_error_errno(SYNTHETIC_ERRNO(EINVAL), "No output specified, use --output=");

        if (arg_certificate_source_type == OPENSSL_CERTIFICATE_SOURCE_FILE) {
                r = parse_path_argument(arg_certificate, /*suppress_root=*/ false, &arg_certificate);
                if (r < 0)
                        return r;
        }

        r = openssl_load_x509_certificate(
                        arg_certificate_source_type,
                        arg_certificate_source,
                        arg_certificate,
                        &certificate);
        if (r < 0)
                return log_error_errno(r, "Failed to load X.509 certificate from %s: %m", arg_certificate);

        _cleanup_close_ int srcfd = open(argv[1], O_RDONLY|O_CLOEXEC);
        if (srcfd < 0)
                return log_error_errno(errno, "Failed to open %s: %m", argv[1]);

        _cleanup_free_ void *pehash = NULL;
        size_t pehashsz;
        r = pe_hash(srcfd, EVP_sha256(), &pehash, &pehashsz);
        if (r < 0)
                return log_error_errno(r, "Failed to hash PE binary %s: %m", argv[0]);

        _cleanup_free_ uint8_t *idcraw = NULL;
        size_t idcrawsz;
        r = spc_indirect_data_configure(pehash, pehashsz, &idcraw, &idcrawsz);
        if (r < 0)
                return r;

        _cleanup_(PKCS7_freep) PKCS7 *p7 = NULL;
        PKCS7_SIGNER_INFO *si = NULL;
        r = pkcs7_configure(certificate, /* private_key= */ NULL, &p7, &si);
        if (r < 0)
                return r;

        _cleanup_(BIO_free_allp) BIO *bio = NULL;
        r = pkcs7_data_configure(p7, idcraw, idcrawsz, &bio);
        if (r < 0)
                return r;

        r = pkcs7_digest_attribute_configure(p7, bio, si);
        if (r < 0)
                return r;

        _cleanup_free_ unsigned char *abuf = NULL;
        int alen = ASN1_item_i2d((ASN1_VALUE *)si->auth_attr, &abuf, ASN1_ITEM_rptr(PKCS7_ATTR_SIGN));
        if (alen < 0 || abuf == NULL)
                return log_error_errno(SYNTHETIC_ERRNO(EIO), "Failed to convert signed attributes ASN.1 to DER: %s",
                                       ERR_error_string(ERR_get_error(), NULL));

        _cleanup_(unlink_and_freep) char *tmp = NULL;
        _cleanup_close_ int dstfd = open_tmpfile_linkable(arg_output, O_RDWR|O_CLOEXEC, &tmp);
        if (dstfd < 0)
                return log_error_errno(r, "Failed to open temporary file: %m");

        r = loop_write(dstfd, abuf, alen);
        if (r < 0)
                return log_error_errno(r, "Failed to write DER-encoded signed attributes blob to temporary file: %m");

        r = link_tmpfile(dstfd, tmp, arg_output, LINK_TMPFILE_REPLACE|LINK_TMPFILE_SYNC);
        if (r < 0)
                return log_error_errno(r, "Failed to link temporary file to %s: %m", arg_output);

        log_info("Wrote DER-encoded signed attributes blob to %s", arg_output);
        return 0;
}

static int run(int argc, char *argv[]) {
        static const Verb verbs[] = {
                { "help",                     VERB_ANY, VERB_ANY, 0,    help                          },
                { "sign",                     2,        2,        0,    verb_sign                     },
                { "export-signed-attributes", 2,        2,        0,    verb_export_signed_attributes },
                {}
        };
        int r;

        log_setup();

        r = parse_argv(argc, argv);
        if (r <= 0)
                return r;

        return dispatch_verb(argc, argv, verbs, NULL);
}

DEFINE_MAIN_FUNCTION(run);
