#include <stdio.h>
#include <wchar.h>

#include "model.h"

static void mftscan_print_error(MftscanError error_code) {
    const char *detail_text = mftscan_error_detail();
    if (detail_text != NULL && detail_text[0] != '\0') {
        fprintf(stderr, "错误: %s: %s\n", mftscan_error_message(error_code), detail_text);
    } else {
        fprintf(stderr, "错误: %s\n", mftscan_error_message(error_code));
    }
}

int wmain(int argc, wchar_t **argv) {
    MftscanOptions options = { 0 };
    MftscanContext context;
    MftscanScanResult scan_result = { 0 };
    MftscanError error_code;
    bool show_help = false;
    int exit_code = 0;

    (void)SetConsoleOutputCP(CP_UTF8);

    error_code = mftscan_parse_options(argc, argv, &options, &show_help);
    if (error_code != MFTSCAN_OK) {
        mftscan_print_error(error_code);
        if (mftscan_error_detail()[0] == '\0') {
            mftscan_print_help(stderr);
        }
        mftscan_free_options(&options);
        return (int)error_code;
    }

    if (show_help) {
        mftscan_print_help(stdout);
        mftscan_free_options(&options);
        return 0;
    }

    mftscan_context_init(&context);

    error_code = mftscan_scan_volume(&context, &options);
    if (error_code == MFTSCAN_OK) {
        error_code = mftscan_build_results(&context, &options, &scan_result);
    }
    if (error_code == MFTSCAN_OK) {
        error_code = (options.format == MFTSCAN_FORMAT_JSON)
            ? mftscan_output_json(&options, &scan_result)
            : mftscan_output_table(&options, &scan_result);
    }

    if (error_code != MFTSCAN_OK) {
        mftscan_print_error(error_code);
        exit_code = (int)error_code;
    }

    mftscan_free_results(&scan_result);
    mftscan_context_free(&context);
    mftscan_free_options(&options);
    return exit_code;
}
