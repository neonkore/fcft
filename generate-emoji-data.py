#!/usr/bin/env python3

import argparse

from dataclasses import dataclass

parser = argparse.ArgumentParser()
parser.add_argument('input', type=argparse.FileType('r'))
parser.add_argument('output', type=argparse.FileType('w'))
opts = parser.parse_args()


@dataclass
class Emoji:
    codepoint: int
    emoji_presentation: bool = False
    modifier: bool = False
    modifier_base: bool = False
    component: bool = False
    pictographic: bool = False


emojis = {}


for line in opts.input:
    line = line.rstrip()
    if not line:
        continue
    if line[0] == '#':
        continue

    codepoint_or_range, props_and_trash = line.split(';', maxsplit=1)
    props = props_and_trash.split('#', maxsplit=1)[0].strip()

    codepoint_range = tuple(
        map(lambda s: int(s.strip(), 16), codepoint_or_range.split('..')))
    assert len(codepoint_range) in [1, 2]

    if len(codepoint_range) == 1:
        codepoint_range = codepoint_range[0], codepoint_range[0]

    for cp in range(codepoint_range[0], codepoint_range[1] + 1):
        assert cp < (1 << 24), f'codepoint is outside range: 0x{cp:x}'

    if props == 'Emoji':
        for cp in range(codepoint_range[0], codepoint_range[1] + 1):
            assert cp not in emojis
            emojis[cp] = Emoji(codepoint=cp)

    elif props == 'Emoji_Presentation':
        for cp in range(codepoint_range[0], codepoint_range[1] + 1):
            try:
                emojis[cp].emoji_presentation = True
            except KeyError:
                pass

    elif props == 'Emoji_Modifier':
        for cp in range(codepoint_range[0], codepoint_range[1] + 1):
            try:
                emojis[cp].modifier = True
            except KeyError:
                pass

    elif props == 'Emoji_Modifier_Base':
        for cp in range(codepoint_range[0], codepoint_range[1] + 1):
            try:
                emojis[cp].modifier_base = True
            except KeyError:
                pass

    elif props == 'Emoji_Component':
        for cp in range(codepoint_range[0], codepoint_range[1] + 1):
            try:
                emojis[cp].component = True
            except KeyError:
                pass

    elif props == 'Extended_Pictographic':
        for cp in range(codepoint_range[0], codepoint_range[1] + 1):
            try:
                emojis[cp].pictographic = True
            except KeyError:
                pass

# for emoji in emojis.values():
#     print(f'{emoji.codepoint:x} ({chr(emoji.codepoint)}): '
#           f'presentation={"emoji" if emoji.emoji_presentation else "text"}, '
#           f'component={emoji.component}, '
#           f'pictographic={emoji.pictographic}')
# print(f'{len(emojis)} codepoints')
# sys.exit(0)

opts.output.write('#pragma once\n')
opts.output.write('#include <stdint.h>\n')
opts.output.write('#include <stdbool.h>\n')
opts.output.write('\n')
opts.output.write('struct emoji {\n')
opts.output.write('    bool emoji_presentation:1;\n')
opts.output.write('    bool modifier:1;\n')
opts.output.write('    bool modifier_base:1;\n')
opts.output.write('    bool component:1;\n')
opts.output.write('    bool pictographic:1;\n')
opts.output.write('    uint32_t cp:24;\n')
opts.output.write('} __attribute__((packed));\n')
opts.output.write('_Static_assert(sizeof(struct emoji) == 4, "unexpected struct size");\n')
opts.output.write('\n')
opts.output.write('#if defined(FCFT_HAVE_HARFBUZZ)\n')
opts.output.write('\n')
opts.output.write(f'static const struct emoji emojis[{len(emojis)}] = {{\n')

for cp in sorted(emojis.keys()):
    emoji = emojis[cp]
    opts.output.write('    {\n')
    opts.output.write(f'       .emoji_presentation = {"true" if emoji.emoji_presentation else "false"},\n')
    opts.output.write(f'       .modifier = {"true" if emoji.modifier else "false"},\n')
    opts.output.write(f'       .modifier_base = {"true" if emoji.modifier_base else "false"},\n')
    opts.output.write(f'       .component = {"true" if emoji.component else "false"},\n')
    opts.output.write(f'       .pictographic = {"true" if emoji.pictographic else "false"},\n')
    opts.output.write(f'       .cp = 0x{emoji.codepoint:05x},\n')
    opts.output.write('    },\n')

opts.output.write('};\n')

opts.output.write('#else  /* !FCFT_HAVE_HARFBUZZ */\n')
opts.output.write('static const struct emoji emojis[0];\n')
opts.output.write('#endif  /* !FCFT_HAVE_HARFBUZZ */\n')
