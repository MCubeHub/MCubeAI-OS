#!/usr/bin/env python3
"""Generate the Chinese IME tables used by the desktop shell.

Two things are produced in one C file, user/programs/ime_data.c:

  * a 16x16 1 bpp bitmap for every character the IME can commit, rendered
    from the system Song typeface (SimSun). The desktop compositor blends
    these next to the 8x16 Latin UI font, so Chinese text needs no font
    engine at runtime;
  * a pinyin dictionary (single characters ordered by everyday frequency,
    plus a handful of common words) and the syllable list the greedy
    segmenter uses for inputs such as "nihao" -> ni + hao.

Run this on the build host with Pillow and pypinyin installed:

    python tools/gen_ime.py

The output is checked in, so a normal build does not need either package.
"""

import os
import sys

CELL = 16
BYTES_PER_ROW = CELL // 8

# Common characters, roughly in frequency order. Order matters: the first
# entry for a syllable becomes the default candidate.
COMMON = (
    "的一是了我不人在他有这个上们来到时大地为子中你说生国年着就那和要她"
    "出也得里后自以会家可下而过天去能对小多然于心学么之都好看起发当没成"
    "只如事把还用第样道想作种开美总从无情己面最女但现前些所同日手又行意"
    "动方期它头经长儿回位分爱老因很给名法间斯知世什两次使身者被高已亲其"
    "进此话常与活正感见明问力理尔点文几定本公特做外孩相西果走将月十实向"
    "声车全信重三机工物气每并别真打太新比才便夫再书部水像眼等体却加电主"
    "界门利海受听表德少克代员许先口由死安写性马光白或住难望教命花结乐色"
    "更拉东神记处让母父应直字场平报友关放至张认接告入笑内英军候民岁往何"
    "度山觉路带万男边风解叫任金快原吃妈变通师立象数四失满战远格士音轻目"
    "条呢病始达深完今提求清王化空业思切怎非找片罗钱吗语元喜曾离飞科言干"
    "流欢约各即指合反题必该论交终林请医晚制球决传画保读运及则房早院量苦"
    "火布品近坐产答星精视五连司巴奇管类未朋且婚台夜青北队久乎越观落尽形"
    "影红爸百令周吧识步希亚术留市半热送兴造谈容极随演收首根讲整式取照办"
    "强石古华拿计您装似足双妻尼转诉米称丽客南领节衣站黑刻统断福城故历惊"
    "脸选包紧争另建维绝树系伤示愿持千史谁准联妇纪基买志静阿诗独复痛消社"
    "算义竟确酒需单治卡幸兰念举仅钟怕共毛句息功官待究跟穿室易游程号居考"
    "突皮哪费倒价图具刚脑永歌响商礼细专黄块脚味灵改据般破引食仍存众注笔"
    "甚某沉血备习校默务土微娘须试怀料调广苏显赛查密议底列富梦错座参八除"
    "跑亮假印设线温虽掉京初养香停际致阳纸李纳验助激够严证帝饭忘趣支春集"
    "丈木研班普导顿睡展跳获艺六波察群皇段急庭创区奥器谢弟店否害草排背止"
    "组州朝封睛板角况曲馆育忙质河续哥呼若推境遇雨标姐充围案伦护冷警贝著"
    "雪索剧啊船险烟依斗值帮汉慢佛肯闻唱沙局伯族低玩资屋击速顾泪洲团圣旁"
    "堂兵七露园牛哭旅街劳型烈姑陈莫鱼异抱宝权鲁简态级票怪寻杀律胜份汽右"
    "洋范床舞秘午登楼贵吸责例追较职属渐左录丝牙党继托赶章智冲叶胡吉卖坚"
    "喝肉遗救修松临藏担戏善卫药悲敢靠伊村戴词森耳差短祖云规窗散迷油旧适"
    "乡架恩投弹铁博雷府压超负勒杂醒洗采毫嘴毕九冰既状乱景席珍童顶派素脱"
    "农疑练野按犯拍征坏骨余承置臂彩灯巨琴免环暗换技翻束增忍餐洛塞缺忆判"
    "欧层付阵批岛项狗休懂武革良恶恋委拥妙探呀营退摇弄桌熟诺宣银势奖宫忽"
    "套康供优课鸟喊降夏困刘罪亡鞋健模败伴守挥鲜财孤枪禁恐伙杰迹妹籍"
)

PHRASES = (
    "你好 我们 你们 他们 她们 中国 中文 汉字 拼音 输入法 输入 文件 系统 "
    "桌面 电脑 程序 应用 设置 网络 浏览器 编辑器 图片 音乐 视频 时间 日期 "
    "今天 明天 谢谢 再见 世界 学习 工作 生活 朋友 老师 学生 软件 硬件 开发 "
    "测试 命令行 终端 窗口 菜单 开始 早上好 晚安 对不起 没关系 我爱你 再见"
)

FONT_CANDIDATES = [
    r"C:\Windows\Fonts\simsun.ttc",
    r"C:\Windows\Fonts\msyh.ttc",
    "/usr/share/fonts/truetype/arphic/uming.ttc",
    "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
]


def find_font():
    for path in FONT_CANDIDATES:
        if os.path.exists(path):
            return path
    raise SystemExit("no Song/CJK font found; add one to FONT_CANDIDATES")


def utf8_octal(text):
    out = []
    for byte in text.encode("utf-8"):
        out.append("\\%03o" % byte)
    return '"' + "".join(out) + '"'


def c_string(text):
    """ASCII (pinyin) strings are emitted plainly, UTF-8 as octal escapes so
    the generated file stays valid whatever the source encoding is."""
    if all(ord(ch) < 128 for ch in text):
        return '"%s"' % text.replace('\\', '\\\\').replace('"', '\\"')
    return utf8_octal(text)


def render_glyphs(chars):
    from PIL import Image, ImageDraw, ImageFont

    font = ImageFont.truetype(find_font(), CELL)
    glyphs = {}
    for ch in chars:
        img = Image.new("L", (CELL, CELL), 0)
        draw = ImageDraw.Draw(img)
        if hasattr(draw, "textbbox"):
            l, t, r, b = draw.textbbox((0, 0), ch, font=font)
        else:
            w, h = draw.textsize(ch, font=font)
            l, t, r, b = 0, 0, w, h
        x = (CELL - (r - l)) // 2 - l
        y = (CELL - (b - t)) // 2 - t
        draw.text((x, y), ch, font=font, fill=255)

        rows = []
        for gy in range(CELL):
            value = 0
            for gx in range(CELL):
                if img.getpixel((gx, gy)) >= 90:
                    value |= 1 << (CELL - 1 - gx)
            rows.append(value)
        # Two bytes per row, big endian, so the runtime reader is trivial.
        glyphs[ch] = [((v >> 8) & 0xFF, v & 0xFF) for v in rows]
    return glyphs


def main():
    from pypinyin import pinyin, lazy_pinyin, Style

    chars = []
    seen = set()

    def add_char(ch):
        if ch not in seen:
            seen.add(ch)
            chars.append(ch)

    for ch in COMMON:
        if not ch.isspace():
            add_char(ch)

    words = [w for w in PHRASES.split() if w]
    for word in words:
        for ch in word:
            add_char(ch)

    entries = []          # (pinyin, text), phrases first
    for word in words:
        py = "".join(lazy_pinyin(word, style=Style.NORMAL))
        entries.append((py, word))
    for ch in COMMON:
        if ch.isspace():
            continue
        readings = pinyin(ch, style=Style.NORMAL)
        py = readings[0][0] if readings else ""
        if py:
            entries.append((py, ch))

    syllables = sorted({py for py, _ in entries if py.isalpha()},
                       key=lambda s: (-len(s), s))

    glyphs = render_glyphs(chars)

    here = os.path.dirname(os.path.abspath(__file__))
    out_path = os.path.join(here, "..", "user", "programs", "ime_data.c")

    with open(out_path, "w", encoding="utf-8") as out:
        out.write("/* Auto-generated by tools/gen_ime.py -- do not edit by hand.\n"
                  " * 16x16 1 bpp Song glyphs + pinyin dictionary for the desktop IME.\n"
                  " * Bitmaps are row-major, two bytes per row, MSB = leftmost pixel. */\n\n")

        out.write("typedef struct {\n"
                  "    u32 cp;\n"
                  "    u8 bmp[%d];\n"
                  "} ime_glyph_t;\n\n" % (CELL * BYTES_PER_ROW))

        ordered = sorted(glyphs.items(), key=lambda kv: ord(kv[0]))
        out.write("static const ime_glyph_t ime_glyphs[] = {\n")
        for ch, rows in ordered:
            flat = ", ".join("0x%02X" % byte for row in rows for byte in row)
            out.write("    { 0x%04X, { %s } },   /* %s */\n"
                      % (ord(ch), flat, ch))
        out.write("};\n\n")

        out.write("static const u8 *ime_glyph(u32 cp) {\n"
                  "    int lo = 0, hi = (int)(sizeof(ime_glyphs) / sizeof(ime_glyphs[0])) - 1;\n"
                  "    while (lo <= hi) {\n"
                  "        int mid = (lo + hi) / 2;\n"
                  "        if (ime_glyphs[mid].cp == cp) return ime_glyphs[mid].bmp;\n"
                  "        if (ime_glyphs[mid].cp < cp) lo = mid + 1;\n"
                  "        else hi = mid - 1;\n"
                  "    }\n"
                  "    return 0;\n"
                  "}\n\n")

        out.write("typedef struct {\n"
                  "    const char *py;\n"
                  "    const char *text;\n"
                  "} ime_entry_t;\n\n")

        out.write("static const ime_entry_t ime_entries[] = {\n")
        for py, text in entries:
            out.write("    { %s, %s },\n" % (c_string(py), c_string(text)))
        out.write("};\n\n")

        out.write("static const char *const ime_syllables[] = {\n")
        for py in syllables:
            out.write("    \"%s\",\n" % py)
        out.write("};\n")

    print("wrote %s: %d glyphs, %d entries, %d syllables"
          % (out_path, len(ordered), len(entries), len(syllables)))


if __name__ == "__main__":
    sys.exit(main())
