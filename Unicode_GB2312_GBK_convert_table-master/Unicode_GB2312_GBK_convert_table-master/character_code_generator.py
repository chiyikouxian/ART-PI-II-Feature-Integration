#coding = utf-8

def code_test():
    # “啊”的gb2312编码为0xB0A1，utf-8编码为0xE5958A，unicode为0x554A
    # 直接使用print打印byte有非常强的迷惑性，因为print会将byte数组里面可打印的字符打印出来，其余不可打印的字符则以16进制的形式打印出来
    # 以此为例.encode("unicode_escape")打印的结果为b'\\u554a'，表面上看起来像以16进制打印的数，实际上它是一个“\u554a”6字节的字符串！
    # 对于utf-16，其中前4个字节是0xFFFE，为BOM头，后2个字节是unicode编码，字节序与BOM头对应
    # 对于utf-32，其中前4个字节是0xFFFE0000，为BOM头，后4个字节是unicode编码，字节序与BOM头对应
    char_data = "啊"
    print(char_data.encode("gb2312"))
    # b'\xb0\xa1'
    print(char_data.encode("gb2312").hex())
    # b0a1
    print(char_data.encode("utf-8"))
    # b'\xe5\x95\x8a'
    print(char_data.encode("utf-8").hex())
    # e5958a
    print(char_data.encode("utf-16"))
    # b'\xff\xfeJU'
    print(char_data.encode("utf-16").hex())
    # fffe4a55
    print(char_data.encode("utf-16le"))
    # b'JU'
    print(char_data.encode("utf-16le").hex())
    # 4a55
    print(char_data.encode("utf-16be"))
    # b'UJ'
    print(char_data.encode("utf-16be").hex())
    # 554a
    print(char_data.encode("utf-32"))
    # b'\xff\xfe\x00\x00JU\x00\x00'
    print(char_data.encode("utf-32").hex())
    # fffe00004a550000
    print(char_data.encode("utf-32le"))
    # b'JU\x00\x00'
    print(char_data.encode("utf-32le").hex())
    # 4a550000
    print(char_data.encode("utf-32be"))
    # b'\x00\x00UJ'
    print(char_data.encode("utf-32be").hex())
    # 0000554a
    print(char_data.encode("unicode_escape"))
    # b'\\u554a'
    print(char_data.encode("unicode_escape").hex())
    # 5c7535353461


def gb2312():
    byte_data = bytearray(2)
    count = 0
    characters = [] # 创建一个列表用于保存汉字字符
    for i in range(0xa1, 0xf8): # 生成包含前9区的特殊符号在内的GB2312所有字符
    # for i in range(0xb0, 0xf8): # 生成一级汉字与二级汉字
    # for i in range(0xd8, 0xf8): # 生成二级汉字
        byte_data[0] = i
        for j in range(0xa1, 0xff):
            byte_data[1] = j
            try:
                # 由于绝大多数常用汉字都在0x4E00-0x9FA5范围内，直接用utf-16be转int就够了
                char = byte_data.decode("gb2312")
                gb = byte_data
                uc = byte_data.decode("gb2312").encode("utf-16be")
                gb_uc_pair = [int.from_bytes(gb, byteorder="big"), int.from_bytes(uc, byteorder="big"), char]
                characters.append(gb_uc_pair)
                # print(char, " gb:", gb.hex(), " uc:", uc.hex())
            except:
                continue
    
    # 输出文件如果使用gbk编码，python自身的编码兼容性似乎有问题，出现如下错误
    # UnicodeEncodeError: 'gbk' codec can't encode character '\u30fb' in position 20: illegal multibyte sequence
    # 这个字符是汉字全角的点“・”，暂不处理，如需转码就用记事本打开文件，然后保存为gbk编码

    # 默认就是GB2312按顺序输出
    with open("gb2312_to_unicode.c", "w", encoding="utf-8") as f:
        f.write("const uint16_t gb2312_to_unicode[%d][2] = {\n" % len(characters))
        for i in characters:
            f.write("{0x%04x, 0x%04x}, //%s\n" % (i[0], i[1], i[2]))
        f.write("};\n")
    
    # 排序后为unicode顺序输出
    characters.sort(key=lambda x: x[1])
    with open("unicode_to_gb2312.c", "w", encoding="utf-8") as f:
        f.write("const uint16_t unicode_to_gb2312[%d][2] = {\n" % len(characters))
        for i in characters:
            f.write("{0x%04x, 0x%04x}, //%s\n" % (i[1], i[0], i[2]))
        f.write("};\n")


def gbk():
    byte_data = bytearray(2)
    characters = [] # 创建一个列表用于保存汉字字符
    for i in range(0x81, 0xff):
        byte_data[0] = i
        for j in range(0x40, 0xff):
            byte_data[1] = j
            try:
                # 由于绝大多数常用汉字都在0x4E00-0x9FA5范围内，直接用utf-16be转int就够了
                char = byte_data.decode("gbk")
                gb = byte_data
                uc = byte_data.decode("gbk").encode("utf-16be")
                gb_uc_pair = [int.from_bytes(gb, byteorder="big"), int.from_bytes(uc, byteorder="big"), char]
                characters.append(gb_uc_pair)
                # print(char, " gb:", gb.hex(), " uc:", uc.hex())
            except:
                continue

    # 默认就是GBK按顺序输出
    with open("gbk_to_unicode.c", "w", encoding="utf-8") as f:
        f.write("const uint16_t gbk_to_unicode[%d][2] = {\n" % len(characters))
        for i in characters:
            f.write("{0x%04x, 0x%04x}, //%s\n" % (i[0], i[1], i[2]))
        f.write("};\n")
    
    # 排序后为unicode顺序输出
    characters.sort(key=lambda x: x[1])
    with open("unicode_to_gbk.c", "w", encoding="utf-8") as f:
        f.write("const uint16_t unicode_to_gbk[%d][2] = {\n" % len(characters))
        for i in characters:
            f.write("{0x%04x, 0x%04x}, //%s\n" % (i[1], i[0], i[2]))
        f.write("};\n")


def main():
    gb2312()
    gbk()


if (__name__ == "__main__"):
    main()
