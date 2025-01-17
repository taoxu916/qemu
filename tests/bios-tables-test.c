/*
 * Boot order test cases.
 *
 * Copyright (c) 2013 Red Hat Inc.
 *
 * Authors:
 *  Michael S. Tsirkin <mst@redhat.com>,
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

/*
 * How to add or update the tests:
 * Contributor:
 * 1. add empty files for new tables, if any, under tests/data/acpi
 * 2. list any changed files in tests/bios-tables-test-allowed-diff.h
 * 3. commit the above *before* making changes that affect the tables
 * Maintainer:
 * After 1-3 above tests will pass but ignore differences with the expected files.
 * You will also notice that tests/bios-tables-test-allowed-diff.h lists
 * a bunch of files. This is your hint that you need to do the below:
 * 4. Run
 *      make check V=1
 * this will produce a bunch of warnings about differences
 * beween actual and expected ACPI tables. If you have IASL installed,
 * they will also be disassembled so you can look at the disassembled
 * output. If not - disassemble them yourself in any way you like.
 * Look at the differences - make sure they make sense and match what the
 * changes you are merging are supposed to do.
 *
 * 5. From build directory, run:
 *      $(SRC_PATH)/tests/data/acpi/rebuild-expected-aml.sh
 * 6. Now commit any changes.
 * 7. Before doing a pull request, make sure tests/bios-tables-test-allowed-diff.h
 *    is empty - this will ensure following changes to ACPI tables will
 *    be noticed.
 */

#include "qemu/osdep.h"
#include <glib/gstdio.h>
#include "qemu-common.h"
#include "hw/firmware/smbios.h"
#include "qemu/bitmap.h"
#include "acpi-utils.h"
#include "boot-sector.h"

#define MACHINE_PC "pc"
#define MACHINE_Q35 "q35"

#define ACPI_REBUILD_EXPECTED_AML "TEST_ACPI_REBUILD_AML"

typedef struct {
    const char *accel;
    const char *machine;
    const char *variant;
    const char *uefi_fl1;
    const char *uefi_fl2;
    const char *cd;
    const uint64_t ram_start;
    const uint64_t scan_len;
    uint64_t rsdp_addr;
    uint8_t rsdp_table[36 /* ACPI 2.0+ RSDP size */];
    GArray *tables;
    uint32_t smbios_ep_addr;
    struct smbios_21_entry_point smbios_ep_table;
    uint8_t *required_struct_types;
    int required_struct_types_len;
    QTestState *qts;
} test_data;

static char disk[] = "tests/acpi-test-disk-XXXXXX";
static const char *data_dir = "tests/data/acpi";
#ifdef CONFIG_IASL
static const char *iasl = stringify(CONFIG_IASL);
#else
static const char *iasl;
#endif

static bool compare_signature(const AcpiSdtTable *sdt, const char *signature)
{
   return !memcmp(sdt->aml, signature, 4);
}

static void cleanup_table_descriptor(AcpiSdtTable *table)
{
    g_free(table->aml);
    if (table->aml_file &&
        !table->tmp_files_retain &&
        g_strstr_len(table->aml_file, -1, "aml-")) {
        unlink(table->aml_file);
    }
    g_free(table->aml_file);
    g_free(table->asl);
    if (table->asl_file &&
        !table->tmp_files_retain) {
        unlink(table->asl_file);
    }
    g_free(table->asl_file);
}

static void free_test_data(test_data *data)
{
    int i;

    for (i = 0; i < data->tables->len; ++i) {
        cleanup_table_descriptor(&g_array_index(data->tables, AcpiSdtTable, i));
    }

    g_array_free(data->tables, true);
}

static void test_acpi_rsdp_table(test_data *data)
{
    uint8_t *rsdp_table = data->rsdp_table;

    acpi_fetch_rsdp_table(data->qts, data->rsdp_addr, rsdp_table);

    switch (rsdp_table[15 /* Revision offset */]) {
    case 0: /* ACPI 1.0 RSDP */
        /* With rev 1, checksum is only for the first 20 bytes */
        g_assert(!acpi_calc_checksum(rsdp_table,  20));
        break;
    case 2: /* ACPI 2.0+ RSDP */
        /* With revision 2, we have 2 checksums */
        g_assert(!acpi_calc_checksum(rsdp_table, 20));
        g_assert(!acpi_calc_checksum(rsdp_table, 36));
        break;
    default:
        g_assert_not_reached();
    }
}

static void test_acpi_rxsdt_table(test_data *data)
{
    const char *sig = "RSDT";
    AcpiSdtTable rsdt = {};
    int entry_size = 4;
    int addr_off = 16 /* RsdtAddress */;
    uint8_t *ent;

    if (data->rsdp_table[15 /* Revision offset */] != 0) {
        addr_off = 24 /* XsdtAddress */;
        entry_size = 8;
        sig = "XSDT";
    }
    /* read [RX]SDT table */
    acpi_fetch_table(data->qts, &rsdt.aml, &rsdt.aml_len,
                     &data->rsdp_table[addr_off], entry_size, sig, true);

    /* Load all tables and add to test list directly RSDT referenced tables */
    ACPI_FOREACH_RSDT_ENTRY(rsdt.aml, rsdt.aml_len, ent, entry_size) {
        AcpiSdtTable ssdt_table = {};

        acpi_fetch_table(data->qts, &ssdt_table.aml, &ssdt_table.aml_len, ent,
                         entry_size, NULL, true);
        /* Add table to ASL test tables list */
        g_array_append_val(data->tables, ssdt_table);
    }
    cleanup_table_descriptor(&rsdt);
}

static void test_acpi_fadt_table(test_data *data)
{
    /* FADT table is 1st */
    AcpiSdtTable table = g_array_index(data->tables, typeof(table), 0);
    uint8_t *fadt_aml = table.aml;
    uint32_t fadt_len = table.aml_len;
    uint32_t val;
    int dsdt_offset = 40 /* DSDT */;
    int dsdt_entry_size = 4;

    g_assert(compare_signature(&table, "FACP"));

    /* Since DSDT/FACS isn't in RSDT, add them to ASL test list manually */
    memcpy(&val, fadt_aml + 112 /* Flags */, 4);
    val = le32_to_cpu(val);
    if (!(val & 1UL << 20 /* HW_REDUCED_ACPI */)) {
        acpi_fetch_table(data->qts, &table.aml, &table.aml_len,
                         fadt_aml + 36 /* FIRMWARE_CTRL */, 4, "FACS", false);
        g_array_append_val(data->tables, table);
    }

    memcpy(&val, fadt_aml + dsdt_offset, 4);
    val = le32_to_cpu(val);
    if (!val) {
        dsdt_offset = 140 /* X_DSDT */;
        dsdt_entry_size = 8;
    }
    acpi_fetch_table(data->qts, &table.aml, &table.aml_len,
                     fadt_aml + dsdt_offset, dsdt_entry_size, "DSDT", true);
    g_array_append_val(data->tables, table);

    memset(fadt_aml + 36, 0, 4); /* sanitize FIRMWARE_CTRL ptr */
    memset(fadt_aml + 40, 0, 4); /* sanitize DSDT ptr */
    if (fadt_aml[8 /* FADT Major Version */] >= 3) {
        memset(fadt_aml + 132, 0, 8); /* sanitize X_FIRMWARE_CTRL ptr */
        memset(fadt_aml + 140, 0, 8); /* sanitize X_DSDT ptr */
    }

    /* update checksum */
    fadt_aml[9 /* Checksum */] = 0;
    fadt_aml[9 /* Checksum */] -= acpi_calc_checksum(fadt_aml, fadt_len);
}

static void dump_aml_files(test_data *data, bool rebuild)
{
    AcpiSdtTable *sdt;
    GError *error = NULL;
    gchar *aml_file = NULL;
    gint fd;
    ssize_t ret;
    int i;

    for (i = 0; i < data->tables->len; ++i) {
        const char *ext = data->variant ? data->variant : "";
        sdt = &g_array_index(data->tables, AcpiSdtTable, i);
        g_assert(sdt->aml);

        if (rebuild) {
            aml_file = g_strdup_printf("%s/%s/%.4s%s", data_dir, data->machine,
                                       sdt->aml, ext);
            fd = g_open(aml_file, O_WRONLY|O_TRUNC|O_CREAT,
                        S_IRUSR|S_IWUSR|S_IRGRP|S_IWGRP|S_IROTH);
            if (fd < 0) {
                perror(aml_file);
            }
            g_assert(fd >= 0);
        } else {
            fd = g_file_open_tmp("aml-XXXXXX", &sdt->aml_file, &error);
            g_assert_no_error(error);
        }

        ret = qemu_write_full(fd, sdt->aml, sdt->aml_len);
        g_assert(ret == sdt->aml_len);

        close(fd);

        g_free(aml_file);
    }
}

static bool load_asl(GArray *sdts, AcpiSdtTable *sdt)
{
    AcpiSdtTable *temp;
    GError *error = NULL;
    GString *command_line = g_string_new(iasl);
    gint fd;
    gchar *out, *out_err;
    gboolean ret;
    int i;

    fd = g_file_open_tmp("asl-XXXXXX.dsl", &sdt->asl_file, &error);
    g_assert_no_error(error);
    close(fd);

    /* build command line */
    g_string_append_printf(command_line, " -p %s ", sdt->asl_file);
    if (compare_signature(sdt, "DSDT") ||
        compare_signature(sdt, "SSDT")) {
        for (i = 0; i < sdts->len; ++i) {
            temp = &g_array_index(sdts, AcpiSdtTable, i);
            if (compare_signature(temp, "DSDT") ||
                compare_signature(temp, "SSDT")) {
                g_string_append_printf(command_line, "-e %s ", temp->aml_file);
            }
        }
    }
    g_string_append_printf(command_line, "-d %s", sdt->aml_file);

    /* pass 'out' and 'out_err' in order to be redirected */
    ret = g_spawn_command_line_sync(command_line->str, &out, &out_err, NULL, &error);
    g_assert_no_error(error);
    if (ret) {
        ret = g_file_get_contents(sdt->asl_file, &sdt->asl,
                                  &sdt->asl_len, &error);
        g_assert(ret);
        g_assert_no_error(error);
        ret = (sdt->asl_len > 0);
    }

    g_free(out);
    g_free(out_err);
    g_string_free(command_line, true);

    return !ret;
}

#define COMMENT_END "*/"
#define DEF_BLOCK "DefinitionBlock ("
#define BLOCK_NAME_END ","

static GString *normalize_asl(gchar *asl_code)
{
    GString *asl = g_string_new(asl_code);
    gchar *comment, *block_name;

    /* strip comments (different generation days) */
    comment = g_strstr_len(asl->str, asl->len, COMMENT_END);
    if (comment) {
        comment += strlen(COMMENT_END);
        while (*comment == '\n') {
            comment++;
        }
        asl = g_string_erase(asl, 0, comment - asl->str);
    }

    /* strip def block name (it has file path in it) */
    if (g_str_has_prefix(asl->str, DEF_BLOCK)) {
        block_name = g_strstr_len(asl->str, asl->len, BLOCK_NAME_END);
        g_assert(block_name);
        asl = g_string_erase(asl, 0,
                             block_name + sizeof(BLOCK_NAME_END) - asl->str);
    }

    return asl;
}

static GArray *load_expected_aml(test_data *data)
{
    int i;
    AcpiSdtTable *sdt;
    GError *error = NULL;
    gboolean ret;
    gsize aml_len;

    GArray *exp_tables = g_array_new(false, true, sizeof(AcpiSdtTable));
    if (getenv("V")) {
        fputc('\n', stderr);
    }
    for (i = 0; i < data->tables->len; ++i) {
        AcpiSdtTable exp_sdt;
        gchar *aml_file = NULL;
        const char *ext = data->variant ? data->variant : "";

        sdt = &g_array_index(data->tables, AcpiSdtTable, i);

        memset(&exp_sdt, 0, sizeof(exp_sdt));

try_again:
        aml_file = g_strdup_printf("%s/%s/%.4s%s", data_dir, data->machine,
                                   sdt->aml, ext);
        if (getenv("V")) {
            fprintf(stderr, "Looking for expected file '%s'\n", aml_file);
        }
        if (g_file_test(aml_file, G_FILE_TEST_EXISTS)) {
            exp_sdt.aml_file = aml_file;
        } else if (*ext != '\0') {
            /* try fallback to generic (extension less) expected file */
            ext = "";
            g_free(aml_file);
            goto try_again;
        }
        g_assert(exp_sdt.aml_file);
        if (getenv("V")) {
            fprintf(stderr, "Using expected file '%s'\n", aml_file);
        }
        ret = g_file_get_contents(aml_file, (gchar **)&exp_sdt.aml,
                                  &aml_len, &error);
        exp_sdt.aml_len = aml_len;
        g_assert(ret);
        g_assert_no_error(error);
        g_assert(exp_sdt.aml);
        if (!exp_sdt.aml_len) {
            fprintf(stderr, "Warning! zero length expected file '%s'\n",
                    aml_file);
        }

        g_array_append_val(exp_tables, exp_sdt);
    }

    return exp_tables;
}

static bool test_acpi_find_diff_allowed(AcpiSdtTable *sdt)
{
    const gchar *allowed_diff_file[] = {
#include "bios-tables-test-allowed-diff.h"
        NULL
    };
    const gchar **f;

    for (f = allowed_diff_file; *f; ++f) {
        if (!g_strcmp0(sdt->aml_file, *f)) {
            return true;
        }
    }
    return false;
}

/* test the list of tables in @data->tables against reference tables */
static void test_acpi_asl(test_data *data)
{
    int i;
    AcpiSdtTable *sdt, *exp_sdt;
    test_data exp_data;
    gboolean exp_err, err, all_tables_match = true;

    memset(&exp_data, 0, sizeof(exp_data));
    exp_data.tables = load_expected_aml(data);
    dump_aml_files(data, false);
    for (i = 0; i < data->tables->len; ++i) {
        GString *asl, *exp_asl;

        sdt = &g_array_index(data->tables, AcpiSdtTable, i);
        exp_sdt = &g_array_index(exp_data.tables, AcpiSdtTable, i);

        if (sdt->aml_len == exp_sdt->aml_len &&
            !memcmp(sdt->aml, exp_sdt->aml, sdt->aml_len)) {
            /* Identical table binaries: no need to disassemble. */
            continue;
        }

        fprintf(stderr,
                "acpi-test: Warning! %.4s binary file mismatch. "
                "Actual [aml:%s], Expected [aml:%s].\n",
                exp_sdt->aml, sdt->aml_file, exp_sdt->aml_file);

        all_tables_match = all_tables_match &&
            test_acpi_find_diff_allowed(exp_sdt);

        /*
         *  don't try to decompile if IASL isn't present, in this case user
         * will just 'get binary file mismatch' warnings and test failure
         */
        if (!iasl) {
            continue;
        }

        err = load_asl(data->tables, sdt);
        asl = normalize_asl(sdt->asl);

        exp_err = load_asl(exp_data.tables, exp_sdt);
        exp_asl = normalize_asl(exp_sdt->asl);

        /* TODO: check for warnings */
        g_assert(!err || exp_err);

        if (g_strcmp0(asl->str, exp_asl->str)) {
            sdt->tmp_files_retain = true;
            if (exp_err) {
                fprintf(stderr,
                        "Warning! iasl couldn't parse the expected aml\n");
            } else {
                exp_sdt->tmp_files_retain = true;
                fprintf(stderr,
                        "acpi-test: Warning! %.4s mismatch. "
                        "Actual [asl:%s, aml:%s], Expected [asl:%s, aml:%s].\n",
                        exp_sdt->aml, sdt->asl_file, sdt->aml_file,
                        exp_sdt->asl_file, exp_sdt->aml_file);
                if (getenv("V")) {
                    const char *diff_cmd = getenv("DIFF");
                    if (diff_cmd) {
                        int ret G_GNUC_UNUSED;
                        char *diff = g_strdup_printf("%s %s %s", diff_cmd,
                            exp_sdt->asl_file, sdt->asl_file);
                        ret = system(diff) ;
                        g_free(diff);
                    } else {
                        fprintf(stderr, "acpi-test: Warning. not showing "
                            "difference since no diff utility is specified. "
                            "Set 'DIFF' environment variable to a preferred "
                            "diff utility and run 'make V=1 check' again to "
                            "see ASL difference.");
                    }
                }
            }
        }
        g_string_free(asl, true);
        g_string_free(exp_asl, true);
    }
    if (!iasl && !all_tables_match) {
        fprintf(stderr, "to see ASL diff between mismatched files install IASL,"
                " rebuild QEMU from scratch and re-run tests with V=1"
                " environment variable set");
    }
    g_assert(all_tables_match);

    free_test_data(&exp_data);
}

static bool smbios_ep_table_ok(test_data *data)
{
    struct smbios_21_entry_point *ep_table = &data->smbios_ep_table;
    uint32_t addr = data->smbios_ep_addr;

    qtest_memread(data->qts, addr, ep_table, sizeof(*ep_table));
    if (memcmp(ep_table->anchor_string, "_SM_", 4)) {
        return false;
    }
    if (memcmp(ep_table->intermediate_anchor_string, "_DMI_", 5)) {
        return false;
    }
    if (ep_table->structure_table_length == 0) {
        return false;
    }
    if (ep_table->number_of_structures == 0) {
        return false;
    }
    if (acpi_calc_checksum((uint8_t *)ep_table, sizeof *ep_table) ||
        acpi_calc_checksum((uint8_t *)ep_table + 0x10,
                           sizeof *ep_table - 0x10)) {
        return false;
    }
    return true;
}

static void test_smbios_entry_point(test_data *data)
{
    uint32_t off;

    /* find smbios entry point structure */
    for (off = 0xf0000; off < 0x100000; off += 0x10) {
        uint8_t sig[] = "_SM_";
        int i;

        for (i = 0; i < sizeof sig - 1; ++i) {
            sig[i] = qtest_readb(data->qts, off + i);
        }

        if (!memcmp(sig, "_SM_", sizeof sig)) {
            /* signature match, but is this a valid entry point? */
            data->smbios_ep_addr = off;
            if (smbios_ep_table_ok(data)) {
                break;
            }
        }
    }

    g_assert_cmphex(off, <, 0x100000);
}

static inline bool smbios_single_instance(uint8_t type)
{
    switch (type) {
    case 0:
    case 1:
    case 2:
    case 3:
    case 16:
    case 32:
    case 127:
        return true;
    default:
        return false;
    }
}

static void test_smbios_structs(test_data *data)
{
    DECLARE_BITMAP(struct_bitmap, SMBIOS_MAX_TYPE+1) = { 0 };
    struct smbios_21_entry_point *ep_table = &data->smbios_ep_table;
    uint32_t addr = le32_to_cpu(ep_table->structure_table_address);
    int i, len, max_len = 0;
    uint8_t type, prv, crt;

    /* walk the smbios tables */
    for (i = 0; i < le16_to_cpu(ep_table->number_of_structures); i++) {

        /* grab type and formatted area length from struct header */
        type = qtest_readb(data->qts, addr);
        g_assert_cmpuint(type, <=, SMBIOS_MAX_TYPE);
        len = qtest_readb(data->qts, addr + 1);

        /* single-instance structs must not have been encountered before */
        if (smbios_single_instance(type)) {
            g_assert(!test_bit(type, struct_bitmap));
        }
        set_bit(type, struct_bitmap);

        /* seek to end of unformatted string area of this struct ("\0\0") */
        prv = crt = 1;
        while (prv || crt) {
            prv = crt;
            crt = qtest_readb(data->qts, addr + len);
            len++;
        }

        /* keep track of max. struct size */
        if (max_len < len) {
            max_len = len;
            g_assert_cmpuint(max_len, <=, ep_table->max_structure_size);
        }

        /* start of next structure */
        addr += len;
    }

    /* total table length and max struct size must match entry point values */
    g_assert_cmpuint(le16_to_cpu(ep_table->structure_table_length), ==,
                     addr - le32_to_cpu(ep_table->structure_table_address));
    g_assert_cmpuint(le16_to_cpu(ep_table->max_structure_size), ==, max_len);

    /* required struct types must all be present */
    for (i = 0; i < data->required_struct_types_len; i++) {
        g_assert(test_bit(data->required_struct_types[i], struct_bitmap));
    }
}

static void test_acpi_one(const char *params, test_data *data)
{
    char *args;
    bool use_uefi = data->uefi_fl1 && data->uefi_fl2;

    if (use_uefi) {
        /*
         * TODO: convert '-drive if=pflash' to new syntax (see e33763be7cd3)
         * when arm/virt boad starts to support it.
         */
        args = g_strdup_printf("-machine %s,accel=%s -nodefaults -nographic "
            "-drive if=pflash,format=raw,file=%s,readonly "
            "-drive if=pflash,format=raw,file=%s,snapshot=on -cdrom %s %s",
            data->machine, data->accel ? data->accel : "kvm:tcg",
            data->uefi_fl1, data->uefi_fl2, data->cd, params ? params : "");

    } else {
        /* Disable kernel irqchip to be able to override apic irq0. */
        args = g_strdup_printf("-machine %s,accel=%s,kernel-irqchip=off "
            "-net none -display none %s "
            "-drive id=hd0,if=none,file=%s,format=raw "
            "-device ide-hd,drive=hd0 ",
             data->machine, data->accel ? data->accel : "kvm:tcg",
             params ? params : "", disk);
    }

    data->qts = qtest_init(args);

    if (use_uefi) {
        g_assert(data->scan_len);
        data->rsdp_addr = acpi_find_rsdp_address_uefi(data->qts,
            data->ram_start, data->scan_len);
    } else {
        boot_sector_test(data->qts);
        data->rsdp_addr = acpi_find_rsdp_address(data->qts);
        g_assert_cmphex(data->rsdp_addr, <, 0x100000);
    }

    data->tables = g_array_new(false, true, sizeof(AcpiSdtTable));
    test_acpi_rsdp_table(data);
    test_acpi_rxsdt_table(data);
    test_acpi_fadt_table(data);

    if (getenv(ACPI_REBUILD_EXPECTED_AML)) {
        dump_aml_files(data, true);
    } else {
        test_acpi_asl(data);
    }

    /*
     * TODO: make SMBIOS tests work with UEFI firmware,
     * Bug on uefi-test-tools to provide entry point:
     * https://bugs.launchpad.net/qemu/+bug/1821884
     */
    if (!use_uefi) {
        test_smbios_entry_point(data);
        test_smbios_structs(data);
    }

    qtest_quit(data->qts);
    g_free(args);
}

static uint8_t base_required_struct_types[] = {
    0, 1, 3, 4, 16, 17, 19, 32, 127
};

static void test_acpi_piix4_tcg(void)
{
    test_data data;

    /* Supplying -machine accel argument overrides the default (qtest).
     * This is to make guest actually run.
     */
    memset(&data, 0, sizeof(data));
    data.machine = MACHINE_PC;
    data.required_struct_types = base_required_struct_types;
    data.required_struct_types_len = ARRAY_SIZE(base_required_struct_types);
    test_acpi_one(NULL, &data);
    free_test_data(&data);
}

static void test_acpi_piix4_tcg_bridge(void)
{
    test_data data;

    memset(&data, 0, sizeof(data));
    data.machine = MACHINE_PC;
    data.variant = ".bridge";
    data.required_struct_types = base_required_struct_types;
    data.required_struct_types_len = ARRAY_SIZE(base_required_struct_types);
    test_acpi_one("-device pci-bridge,chassis_nr=1", &data);
    free_test_data(&data);
}

static void test_acpi_q35_tcg(void)
{
    test_data data;

    memset(&data, 0, sizeof(data));
    data.machine = MACHINE_Q35;
    data.required_struct_types = base_required_struct_types;
    data.required_struct_types_len = ARRAY_SIZE(base_required_struct_types);
    test_acpi_one(NULL, &data);
    free_test_data(&data);
}

static void test_acpi_q35_tcg_bridge(void)
{
    test_data data;

    memset(&data, 0, sizeof(data));
    data.machine = MACHINE_Q35;
    data.variant = ".bridge";
    data.required_struct_types = base_required_struct_types;
    data.required_struct_types_len = ARRAY_SIZE(base_required_struct_types);
    test_acpi_one("-device pci-bridge,chassis_nr=1",
                  &data);
    free_test_data(&data);
}

static void test_acpi_q35_tcg_mmio64(void)
{
    test_data data = {
        .machine = MACHINE_Q35,
        .variant = ".mmio64",
        .required_struct_types = base_required_struct_types,
        .required_struct_types_len = ARRAY_SIZE(base_required_struct_types)
    };

    test_acpi_one("-m 128M,slots=1,maxmem=2G "
                  "-object memory-backend-ram,id=ram0,size=128M "
                  "-numa node,memdev=ram0 "
                  "-device pci-testdev,membar=2G",
                  &data);
    free_test_data(&data);
}

static void test_acpi_piix4_tcg_cphp(void)
{
    test_data data;

    memset(&data, 0, sizeof(data));
    data.machine = MACHINE_PC;
    data.variant = ".cphp";
    test_acpi_one("-smp 2,cores=3,sockets=2,maxcpus=6"
                  " -object memory-backend-ram,id=ram0,size=64M"
                  " -object memory-backend-ram,id=ram1,size=64M"
                  " -numa node,memdev=ram0 -numa node,memdev=ram1"
                  " -numa dist,src=0,dst=1,val=21",
                  &data);
    free_test_data(&data);
}

static void test_acpi_q35_tcg_cphp(void)
{
    test_data data;

    memset(&data, 0, sizeof(data));
    data.machine = MACHINE_Q35;
    data.variant = ".cphp";
    test_acpi_one(" -smp 2,cores=3,sockets=2,maxcpus=6"
                  " -object memory-backend-ram,id=ram0,size=64M"
                  " -object memory-backend-ram,id=ram1,size=64M"
                  " -numa node,memdev=ram0 -numa node,memdev=ram1"
                  " -numa dist,src=0,dst=1,val=21",
                  &data);
    free_test_data(&data);
}

static uint8_t ipmi_required_struct_types[] = {
    0, 1, 3, 4, 16, 17, 19, 32, 38, 127
};

static void test_acpi_q35_tcg_ipmi(void)
{
    test_data data;

    memset(&data, 0, sizeof(data));
    data.machine = MACHINE_Q35;
    data.variant = ".ipmibt";
    data.required_struct_types = ipmi_required_struct_types;
    data.required_struct_types_len = ARRAY_SIZE(ipmi_required_struct_types);
    test_acpi_one("-device ipmi-bmc-sim,id=bmc0"
                  " -device isa-ipmi-bt,bmc=bmc0",
                  &data);
    free_test_data(&data);
}

static void test_acpi_piix4_tcg_ipmi(void)
{
    test_data data;

    /* Supplying -machine accel argument overrides the default (qtest).
     * This is to make guest actually run.
     */
    memset(&data, 0, sizeof(data));
    data.machine = MACHINE_PC;
    data.variant = ".ipmikcs";
    data.required_struct_types = ipmi_required_struct_types;
    data.required_struct_types_len = ARRAY_SIZE(ipmi_required_struct_types);
    test_acpi_one("-device ipmi-bmc-sim,id=bmc0"
                  " -device isa-ipmi-kcs,irq=0,bmc=bmc0",
                  &data);
    free_test_data(&data);
}

static void test_acpi_q35_tcg_memhp(void)
{
    test_data data;

    memset(&data, 0, sizeof(data));
    data.machine = MACHINE_Q35;
    data.variant = ".memhp";
    test_acpi_one(" -m 128,slots=3,maxmem=1G"
                  " -object memory-backend-ram,id=ram0,size=64M"
                  " -object memory-backend-ram,id=ram1,size=64M"
                  " -numa node,memdev=ram0 -numa node,memdev=ram1"
                  " -numa dist,src=0,dst=1,val=21",
                  &data);
    free_test_data(&data);
}

static void test_acpi_piix4_tcg_memhp(void)
{
    test_data data;

    memset(&data, 0, sizeof(data));
    data.machine = MACHINE_PC;
    data.variant = ".memhp";
    test_acpi_one(" -m 128,slots=3,maxmem=1G"
                  " -object memory-backend-ram,id=ram0,size=64M"
                  " -object memory-backend-ram,id=ram1,size=64M"
                  " -numa node,memdev=ram0 -numa node,memdev=ram1"
                  " -numa dist,src=0,dst=1,val=21",
                  &data);
    free_test_data(&data);
}

static void test_acpi_q35_tcg_numamem(void)
{
    test_data data;

    memset(&data, 0, sizeof(data));
    data.machine = MACHINE_Q35;
    data.variant = ".numamem";
    test_acpi_one(" -object memory-backend-ram,id=ram0,size=128M"
                  " -numa node -numa node,memdev=ram0", &data);
    free_test_data(&data);
}

static void test_acpi_piix4_tcg_numamem(void)
{
    test_data data;

    memset(&data, 0, sizeof(data));
    data.machine = MACHINE_PC;
    data.variant = ".numamem";
    test_acpi_one(" -object memory-backend-ram,id=ram0,size=128M"
                  " -numa node -numa node,memdev=ram0", &data);
    free_test_data(&data);
}

static void test_acpi_tcg_dimm_pxm(const char *machine)
{
    test_data data;

    memset(&data, 0, sizeof(data));
    data.machine = machine;
    data.variant = ".dimmpxm";
    test_acpi_one(" -machine nvdimm=on,nvdimm-persistence=cpu"
                  " -smp 4,sockets=4"
                  " -m 128M,slots=3,maxmem=1G"
                  " -object memory-backend-ram,id=ram0,size=32M"
                  " -object memory-backend-ram,id=ram1,size=32M"
                  " -object memory-backend-ram,id=ram2,size=32M"
                  " -object memory-backend-ram,id=ram3,size=32M"
                  " -numa node,memdev=ram0,nodeid=0"
                  " -numa node,memdev=ram1,nodeid=1"
                  " -numa node,memdev=ram2,nodeid=2"
                  " -numa node,memdev=ram3,nodeid=3"
                  " -numa cpu,node-id=0,socket-id=0"
                  " -numa cpu,node-id=1,socket-id=1"
                  " -numa cpu,node-id=2,socket-id=2"
                  " -numa cpu,node-id=3,socket-id=3"
                  " -object memory-backend-ram,id=ram4,size=128M"
                  " -object memory-backend-ram,id=nvm0,size=128M"
                  " -device pc-dimm,id=dimm0,memdev=ram4,node=1"
                  " -device nvdimm,id=dimm1,memdev=nvm0,node=2",
                  &data);
    free_test_data(&data);
}

static void test_acpi_q35_tcg_dimm_pxm(void)
{
    test_acpi_tcg_dimm_pxm(MACHINE_Q35);
}

static void test_acpi_piix4_tcg_dimm_pxm(void)
{
    test_acpi_tcg_dimm_pxm(MACHINE_PC);
}

static void test_acpi_virt_tcg_memhp(void)
{
    test_data data = {
        .machine = "virt",
        .accel = "tcg",
        .uefi_fl1 = "pc-bios/edk2-aarch64-code.fd",
        .uefi_fl2 = "pc-bios/edk2-arm-vars.fd",
        .cd = "tests/data/uefi-boot-images/bios-tables-test.aarch64.iso.qcow2",
        .ram_start = 0x40000000ULL,
        .scan_len = 256ULL * 1024 * 1024,
    };

    data.variant = ".memhp";
    test_acpi_one(" -cpu cortex-a57"
                  " -m 256M,slots=3,maxmem=1G"
                  " -object memory-backend-ram,id=ram0,size=128M"
                  " -object memory-backend-ram,id=ram1,size=128M"
                  " -numa node,memdev=ram0 -numa node,memdev=ram1"
                  " -numa dist,src=0,dst=1,val=21",
                  &data);

    free_test_data(&data);

}

static void test_acpi_virt_tcg_numamem(void)
{
    test_data data = {
        .machine = "virt",
        .accel = "tcg",
        .uefi_fl1 = "pc-bios/edk2-aarch64-code.fd",
        .uefi_fl2 = "pc-bios/edk2-arm-vars.fd",
        .cd = "tests/data/uefi-boot-images/bios-tables-test.aarch64.iso.qcow2",
        .ram_start = 0x40000000ULL,
        .scan_len = 128ULL * 1024 * 1024,
    };

    data.variant = ".numamem";
    test_acpi_one(" -cpu cortex-a57"
                  " -object memory-backend-ram,id=ram0,size=128M"
                  " -numa node,memdev=ram0",
                  &data);

    free_test_data(&data);

}

static void test_acpi_tcg_acpi_hmat(const char *machine)
{
    test_data data;

    memset(&data, 0, sizeof(data));
    data.machine = machine;
    data.variant = ".acpihmat";
    test_acpi_one(" -machine hmat=on"
                  " -smp 2,sockets=2"
                  " -m 128M,slots=2,maxmem=1G"
                  " -object memory-backend-ram,size=64M,id=m0"
                  " -object memory-backend-ram,size=64M,id=m1"
                  " -numa node,nodeid=0,memdev=m0"
                  " -numa node,nodeid=1,memdev=m1,initiator=0"
                  " -numa cpu,node-id=0,socket-id=0"
                  " -numa cpu,node-id=0,socket-id=1"
                  " -numa hmat-lb,initiator=0,target=0,hierarchy=memory,"
                  "data-type=access-latency,latency=1"
                  " -numa hmat-lb,initiator=0,target=0,hierarchy=memory,"
                  "data-type=access-bandwidth,bandwidth=65534M"
                  " -numa hmat-lb,initiator=0,target=1,hierarchy=memory,"
                  "data-type=access-latency,latency=65534"
                  " -numa hmat-lb,initiator=0,target=1,hierarchy=memory,"
                  "data-type=access-bandwidth,bandwidth=32767M"
                  " -numa hmat-cache,node-id=0,size=10K,level=1,"
                  "associativity=direct,policy=write-back,line=8"
                  " -numa hmat-cache,node-id=1,size=10K,level=1,"
                  "associativity=direct,policy=write-back,line=8",
                  &data);
    free_test_data(&data);
}

static void test_acpi_q35_tcg_acpi_hmat(void)
{
    test_acpi_tcg_acpi_hmat(MACHINE_Q35);
}

static void test_acpi_piix4_tcg_acpi_hmat(void)
{
    test_acpi_tcg_acpi_hmat(MACHINE_PC);
}

static void test_acpi_virt_tcg(void)
{
    test_data data = {
        .machine = "virt",
        .accel = "tcg",
        .uefi_fl1 = "pc-bios/edk2-aarch64-code.fd",
        .uefi_fl2 = "pc-bios/edk2-arm-vars.fd",
        .cd = "tests/data/uefi-boot-images/bios-tables-test.aarch64.iso.qcow2",
        .ram_start = 0x40000000ULL,
        .scan_len = 128ULL * 1024 * 1024,
    };

    test_acpi_one("-cpu cortex-a57", &data);
    free_test_data(&data);
}

int main(int argc, char *argv[])
{
    const char *arch = qtest_get_arch();
    int ret;

    g_test_init(&argc, &argv, NULL);

    if (strcmp(arch, "i386") == 0 || strcmp(arch, "x86_64") == 0) {
        ret = boot_sector_init(disk);
        if (ret) {
            return ret;
        }

        qtest_add_func("acpi/piix4", test_acpi_piix4_tcg);
        qtest_add_func("acpi/piix4/bridge", test_acpi_piix4_tcg_bridge);
        qtest_add_func("acpi/q35", test_acpi_q35_tcg);
        qtest_add_func("acpi/q35/bridge", test_acpi_q35_tcg_bridge);
        qtest_add_func("acpi/q35/mmio64", test_acpi_q35_tcg_mmio64);
        qtest_add_func("acpi/piix4/ipmi", test_acpi_piix4_tcg_ipmi);
        qtest_add_func("acpi/q35/ipmi", test_acpi_q35_tcg_ipmi);
        qtest_add_func("acpi/piix4/cpuhp", test_acpi_piix4_tcg_cphp);
        qtest_add_func("acpi/q35/cpuhp", test_acpi_q35_tcg_cphp);
        qtest_add_func("acpi/piix4/memhp", test_acpi_piix4_tcg_memhp);
        qtest_add_func("acpi/q35/memhp", test_acpi_q35_tcg_memhp);
        qtest_add_func("acpi/piix4/numamem", test_acpi_piix4_tcg_numamem);
        qtest_add_func("acpi/q35/numamem", test_acpi_q35_tcg_numamem);
        qtest_add_func("acpi/piix4/dimmpxm", test_acpi_piix4_tcg_dimm_pxm);
        qtest_add_func("acpi/q35/dimmpxm", test_acpi_q35_tcg_dimm_pxm);
        qtest_add_func("acpi/piix4/acpihmat", test_acpi_piix4_tcg_acpi_hmat);
        qtest_add_func("acpi/q35/acpihmat", test_acpi_q35_tcg_acpi_hmat);
    } else if (strcmp(arch, "aarch64") == 0) {
        qtest_add_func("acpi/virt", test_acpi_virt_tcg);
        qtest_add_func("acpi/virt/numamem", test_acpi_virt_tcg_numamem);
        qtest_add_func("acpi/virt/memhp", test_acpi_virt_tcg_memhp);
    }
    ret = g_test_run();
    boot_sector_cleanup(disk);
    return ret;
}
