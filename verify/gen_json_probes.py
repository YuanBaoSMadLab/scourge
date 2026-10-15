# -*- coding: utf-8 -*-
# probe_json_edge.sg 生成器：绕开 bash heredoc 的反斜杠损坏
import io

L = []
L.append('def main() -> int:')
# JSON 源文本（scourge 源码字面量内层）： "line1\nline2\tA\u0041\u4e2d\u6587 quote:\" back:\\/\b\f\r"
# scourge 源码里每个反斜杠写成 \\ 、引号写成 \"
json_src = r'"line1\nline2\tA\u0041\u4e2d\u6587 quote:\" back:\\/\b\f\r"'
sg_lit = json_src.replace('\\', '\\\\').replace('"', '\\"')
L.append('    esc: Json = json_decode("' + sg_lit + '")')
L.append('    print(esc.as_str())')
L.append('    print(esc.dump())')
L.append('    neg: Json = json_decode("[ -42, 0, 9007199254740993, 1.5e3, -2.5e-3, 1e2 ]")')
L.append('    print(neg.at(0).as_int())')
L.append('    print(neg.at(1).as_int())')
L.append('    print(neg.at(2).as_int())')
L.append('    print(neg.at(3).as_float())')
L.append('    print(neg.at(4).as_float())')
L.append('    print(neg.at(5).is_float())')
L.append('    print(neg.at(5).dump())')
L.append('    e: Json = json_decode("{}")')
L.append('    print(e.is_obj())')
L.append('    print(e.len())')
L.append('    print(e.dump())')
L.append('    a: Json = json_decode("[]")')
L.append('    print(a.len())')
L.append('    print(a.dump())')
L.append('    dup: Json = json_decode("{\\"k\\": 1, \\"k\\": 2}")')
L.append('    print(dup.get("k").as_int())')
L.append('    print(dup.dump())')
L.append('    print(esc)')
L.append('    deep: Json = json_decode("[[[[\\"deep\\"]]]]")')
L.append('    print(deep.at(0).at(0).at(0).at(0).as_str())')
L.append('    num: Json = json_decode("42")')
L.append('    print(num.is_int())')
L.append('    print(num.as_int())')
L.append('    print(num.dump())')
L.append('    return 0')

io.open('codebuddytest/probe_json_edge.sg', 'w', encoding='utf-8', newline='\n').write('\n'.join(L) + '\n')

# panic 探针 3 个
p1 = 'def main() -> int:\n' \
     '    j: Json = json_decode("{\\"a\\": 1")\n' \
     '    return 0\n'
p2 = 'def main() -> int:\n' \
     '    j: Json = json_decode("[1, \\"x\\"]")\n' \
     '    print(j.at(0).as_str())\n' \
     '    return 0\n'
p3 = 'def main() -> int:\n' \
     '    j: Json = json_decode("[1]")\n' \
     '    print(j.at(5).as_int())\n' \
     '    return 0\n'
p4 = 'def main() -> int:\n' \
     '    j: Json = json_decode("[1]")\n' \
     '    print(j.at(0).len())\n' \
     '    return 0\n'
io.open('codebuddytest/probe_json_panic_bad.sg', 'w', encoding='utf-8', newline='\n').write(p1)
io.open('codebuddytest/probe_json_panic_asint.sg', 'w', encoding='utf-8', newline='\n').write(p2)
io.open('codebuddytest/probe_json_panic_at.sg', 'w', encoding='utf-8', newline='\n').write(p3)
io.open('codebuddytest/probe_json_panic_len.sg', 'w', encoding='utf-8', newline='\n').write(p4)

# 负探针：类型拦截（check 必失败）
n1 = '''def main() -> int:
    j: Json = json_decode(42)
    return 0
'''
io.open('codebuddytest/neg_json_type.sg', 'w', encoding='utf-8', newline='\n').write(n1)

# acc 探针：全方法面
acc = '''def main() -> int:
    j: Json = json_decode("{\\"name\\": \\"scourge\\", \\"ver\\": 1, \\"pi\\": 3.25, \\"tags\\": [\\"a\\", \\"b\\"], \\"flag\\": true, \\"none\\": null}")
    print(j.is_obj())
    print(j.is_list())
    name: Json = j.get("name")
    print(name.is_str())
    print(name.as_str())
    print(j.get("ver").is_int())
    print(j.get("ver").as_int())
    print(j.get("pi").as_float())
    print(j.get("flag").as_bool())
    print(j.get("none").is_null())
    tags: Json = j.get("tags")
    print(tags.is_list())
    print(tags.len())
    print(tags.at(0).as_str())
    print(tags.at(1).as_str())
    print(j.len())
    ks: List[str] = j.keys()
    print(ks)
    print(j.dump())
    print(json_decode("[10, 20, 30]").at(2).as_int())
    return 0
'''
io.open('codebuddytest/probe_json_acc.sg', 'w', encoding='utf-8', newline='\n').write(acc)
print('probes written')
