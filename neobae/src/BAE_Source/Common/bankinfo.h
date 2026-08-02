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

// Embedded bank metadata generated from BXBanks.xml / Banks.xml.
// This replaces runtime XML parsing to simplify distribution.
// If you need to regenerate, edit this table and rebuild.
#ifndef MINIBAE_GUI_BANKINFO_H
#define MINIBAE_GUI_BANKINFO_H

/* Optional flags for known banks (OR together). */
#define BANKINFO_FLAG_NONE       0u
/* DLS: treat as mobileBAE even without a 'pgal' chunk (quirks + badge). */
#define BANKINFO_FLAG_MOBILEBAE  1u

typedef struct BankInfo {
    const char *name;     // friendly name
    const char *sha1;     // lowercase hex sha1 of bank file contents
    unsigned flags;       // BANKINFO_FLAG_*
} BankInfo;

static const BankInfo kBanks[] = {
    {"Beatnik Standard (v2.4.3, from Java)",       "003232ab5a418f34007c525d66e8220993fe62f8", BANKINFO_FLAG_NONE},
    {"WebTV Plus (v2.1.0, build-3454)",            "1780c86aeb5a3e3d85ee02d8f20200a8e3629f4b", BANKINFO_FLAG_NONE},
    {"WebTV Classic (bootrom 105)",                "1bcf0aff4ef3bcab16ed7c1437b38734a5c7f1f2", BANKINFO_FLAG_NONE},
    {"WebTV Plus (wtv-music, uncomp)",             "276bf7f476c17d5c32620e114e9f0ad5758dd80e", BANKINFO_FLAG_NONE},
    {"HruskaNokia (Rev 1, unfinished)",            "2a9b4813ff7df35db57da56233f4cbd6ca9a648d", BANKINFO_FLAG_NONE},
    {"Java v1.5 Mid Soundbank",                    "3e1b1f2374d3f47a5e4b4a1399a735396925700f", BANKINFO_FLAG_NONE},
    {"BeOS Big Synth",                             "41a0ec1c18e92d31645d4d70fa6af7b92660f4f0", BANKINFO_FLAG_NONE},
    {"SalterNokia (Rev 4)",                        "60ce5e78c62dac0ccbf68c1c768e5c0d5d1c1faa", BANKINFO_FLAG_NONE},
    {"Nokia N-Gage (Mango)",                       "6b89950cd76e0542ab2575c69ab3448d0f0c17b5", BANKINFO_FLAG_NONE},
    {"WebTV Plus (wtv-music)",                     "7fa3168dae8ee9037a3935bb7cd93f26b6ce996e", BANKINFO_FLAG_NONE},
    {"Nokia 7650 (Large)",                         "82f15ab9938069e07db9181934970a1d6be37c01", BANKINFO_FLAG_NONE},
    {"BeOS Patches",                               "835f57896955b03561b33ff02c957bda6d2a938d", BANKINFO_FLAG_NONE},
    {"Chippy",                                     "8664e092ecfd66e0adbbee871c9ff5bbbbaf0494", BANKINFO_FLAG_NONE},
    {"SalterNokia (Rev 3)",                        "88f1d4813fb86ac7891dadd12c142ceff878e2f0", BANKINFO_FLAG_NONE},
    {"Danger Hiptop 1.1",                          "8bb26fc66aecf14e2dfa48d2123e2576863fa2ab", BANKINFO_FLAG_NONE},
    {"Nokia 5140i (Idefix)",                       "9d887700b5489462f7f38f8ea094bcba0c850234", BANKINFO_FLAG_NONE},
    {"Sony Ericsson P900",                         "a3f90a5a94b85aa686ff54bdfe899f7aa977ada9", BANKINFO_FLAG_NONE},
    {"Nano Bank",                                  "a6715b2c1ec23fd573823f4c8a18e0933aaeea19", BANKINFO_FLAG_NONE},
    {"Beatnik Pro (v3.0.0, uncomp)",               "ae4d88373aeca8ee2ff5ced8d6b76bbfeb91c8cc", BANKINFO_FLAG_NONE},
    {"Beatnik Standard (v1.1.1, uncomp)",          "b4521cd38c123c272757c58fe058944a4e29c90c", BANKINFO_FLAG_NONE},
    {"HruskaNokia (Rev 5)",                        "b650b8ce69e470ed771b5a5df7aa3a529193d073", BANKINFO_FLAG_NONE},
    {"Sony Ericsson P800",                         "c87ef17f4ed38fd994c7c38aa82ff94121cfa5f2", BANKINFO_FLAG_NONE},
    {"Beatnik Standard (v2.4.6, from editor)",     "c981e81b19959c981f2f46477abcbfc0df1af5a1", BANKINFO_FLAG_NONE},
    {"SalterNokia (Rev 8a EQ)",                    "d8b74e98310ea891331a18dbcfd9f8e8e3b05af4", BANKINFO_FLAG_NONE},
    {"SalterNokia (Rev 8a)",                       "df1225ef07b615638513f88801aedf8a6a8767b3", BANKINFO_FLAG_NONE},
    {"Nokia 3510 (Chippy)",                        "e0ca456952793b8d5ee16c598a0e48712a912bba", BANKINFO_FLAG_NONE},
    {"Nokia N-Gage (Mango, 020719 version)",       "e2d833cd916b6d55f52924903942e33e7955978a", BANKINFO_FLAG_NONE},
    {"Nokia N-Gage (Mango, 020812 version)",       "ea1101f9456ec10985079af7ca83e074cd61cbbc", BANKINFO_FLAG_NONE},
    {"Java v1.5 Deluxe Soundbank",                 "e8109db5ea8133a6e1d429a8bef4a1a8cb1396e7", BANKINFO_FLAG_NONE},
    {"WebTV Classic (Alpha)",                      "ee5a505aaeee40a84b04360eadc78ce8496e7c2b", BANKINFO_FLAG_NONE},
    {"Micro Bank",                                 "f8438602a0254ee6d224f6051b33804f076ad926", BANKINFO_FLAG_NONE},
    {"Samsung TouchWiz",                           "49c36607e5540e54a088d3d1337f667f5c0206e5", BANKINFO_FLAG_NONE},
    {"Charlie Bank",                               "1dd3935168ea167df74531df7912432f53d881e0", BANKINFO_FLAG_MOBILEBAE}
};

static const int kBankCount = (int)(sizeof(kBanks)/sizeof(kBanks[0]));

#endif // MINIBAE_GUI_BANKINFO_H
