/*
 * © 2021–2026 zefie
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program. If not, see <https://www.gnu.org/licenses/>.
 */

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "mthc_decomp.h"

static void print_usage(char const *program_name)
{
    fprintf(stderr,
            "MThc decompressor\n"
            "Usage: %s [options] <input.mid>\n"
            "\n"
            "Options:\n"
            "  -v, --verbose              Print payload preview\n"
            "  --extract-payload <path>   Write raw MThp payload to a file\n"
            "  --decompress <path>        Decompress MThc file and write MIDI to path\n",
            program_name);
}

int main(int argc, char *argv[])
{
    char const *input_path = NULL;
    char const *extract_path = NULL;
    char const *decompress_path = NULL;
    bool verbose = false;

    for (int i = 1; i < argc; ++i)
    {
        char const *arg = argv[i];

        if (strcmp(arg, "-v") == 0 || strcmp(arg, "--verbose") == 0)
        {
            verbose = true;
            continue;
        }

        if (strcmp(arg, "--extract-payload") == 0)
        {
            if ((i + 1) >= argc)
            {
                print_usage(argv[0]);
                return 1;
            }
            extract_path = argv[++i];
            continue;
        }

        if (strcmp(arg, "--decompress") == 0)
        {
            if ((i + 1) >= argc)
            {
                print_usage(argv[0]);
                return 1;
            }
            decompress_path = argv[++i];
            continue;
        }

        if (strcmp(arg, "-h") == 0 || strcmp(arg, "--help") == 0)
        {
            print_usage(argv[0]);
            return 0;
        }

        if (!input_path)
        {
            input_path = arg;
            continue;
        }

        fprintf(stderr, "Error: unexpected argument '%s'\n", arg);
        print_usage(argv[0]);
        return 1;
    }

    if (!input_path)
    {
        print_usage(argv[0]);
        return 1;
    }

    return mthc_process_file(input_path, extract_path, decompress_path, verbose);
}
