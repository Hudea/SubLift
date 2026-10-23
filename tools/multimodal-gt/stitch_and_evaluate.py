#!/usr/bin/env python3
"""
Standalone Multimodal GT Stitcher & Evaluator.

1. Stitches one or more multimodal transcription JSON batches (`[{"start_frame": int, "end_frame": int, "text": str}]`)
   into a standard `.srt` file using the half-open interval mapping `[start_frame * step_ms, (end_frame + 1) * step_ms]`.
2. Optionally compares the generated `.srt` against a reference Ground Truth `.srt`.
3. Supports `--reproduce-zootopia-ab` to replay and verify the Phase 21 Zootopia_clip_1080p A/B blind experiment.
"""
from __future__ import annotations

import argparse
import json
import re
from pathlib import Path
from typing import Any


# Recorded blind multimodal outputs from the Zootopia_clip_1080p.mp4 A/B experiment
ZOOTOPIA_SCHEME_A_BATCHES: list[list[dict[str, Any]]] = [
    [
        {"start_frame": 260, "end_frame": 287, "text": "我流血啦…我死啦"},
        {"start_frame": 288, "end_frame": 299, "text": "(前情提要…)"},
        {"start_frame": 300, "end_frame": 314, "text": "好啦，你也演太大了"},
        {"start_frame": 316, "end_frame": 327, "text": "可以收工了"},
        {"start_frame": 329, "end_frame": 338, "text": "(前情蹄要…)"},
        {"start_frame": 340, "end_frame": 359, "text": "可以收工啦，谢啰，长舌妇"},
        {"start_frame": 360, "end_frame": 372, "text": "你整个突破盲肠了"},
        {"start_frame": 374, "end_frame": 402, "text": "我嫁祸给狮明德，也能嫁祸给你"},
        {"start_frame": 403, "end_frame": 424, "text": "尽管说你的片面之词吧"},
        {"start_frame": 426, "end_frame": 439, "text": "事实上…"},
        {"start_frame": 440, "end_frame": 463, "text": "我会利用动物方城市所有猎食者"},
        {"start_frame": 465, "end_frame": 472, "text": "来保持现状"},
        {"start_frame": 474, "end_frame": 492, "text": "是你的片面之词"},
        {"start_frame": 499, "end_frame": 519, "text": "这叫说话的艺术，宝贝"},
        {"start_frame": 521, "end_frame": 530, "text": "砰"},
        {"start_frame": 560, "end_frame": 567, "text": "不寻常的拍档…"},
        {"start_frame": 569, "end_frame": 579, "text": "(动物方城市新闻台)"},
        {"start_frame": 581, "end_frame": 598, "text": "哈茱蒂，本市第一位兔警员"},
        {"start_frame": 600, "end_frame": 625, "text": "跟胡尼克，一位平凡的狐狸"},
        {"start_frame": 627, "end_frame": 644, "text": "(前市长杨咩咩入狱)"},
        {"start_frame": 646, "end_frame": 659, "text": "一起揭穿了杨咩咩市长的阴谋"},
        {"start_frame": 661, "end_frame": 682, "text": "用红萝卜玩具录音笔"},
        {"start_frame": 684, "end_frame": 698, "text": "我也能嫁祸给你"},
        {"start_frame": 742, "end_frame": 772, "text": "后来，胡尼克加入ZPD警局"},
        {"start_frame": 774, "end_frame": 794, "text": "成为动物方城市史上"},
        {"start_frame": 796, "end_frame": 807, "text": "(动物方城市警察学校)"},
        {"start_frame": 808, "end_frame": 824, "text": "第一组兔狐搭档"},
        {"start_frame": 838, "end_frame": 862, "text": "新任市长暨前演员马立夯"},
        {"start_frame": 864, "end_frame": 878, "text": "在动物方城市气候墙"},
        {"start_frame": 880, "end_frame": 905, "text": "发明一百周年庆祝活动上"},
        {"start_frame": 907, "end_frame": 934, "text": "表扬他们两位"},
        {"start_frame": 936, "end_frame": 958, "text": "气候墙让各种环境和动物"},
        {"start_frame": 960, "end_frame": 969, "text": "(鱼镇市场，克拉克比目鱼)"},
        {"start_frame": 971, "end_frame": 982, "text": "能在一个城市中共存"},
        {"start_frame": 983, "end_frame": 1016, "text": "这是我们建城百年纪念"},
        {"start_frame": 1018, "end_frame": 1030, "text": "如果出身卑微的乡下兔子"},
    ],
    [
        {"start_frame": 1029, "end_frame": 1038, "text": "如果出身卑微的乡下兔子"},
        {"start_frame": 1040, "end_frame": 1064, "text": "跟可能有案底的奸诈狐狸"},
        {"start_frame": 1066, "end_frame": 1093, "text": "能不顾他们惊人的差异"},
        {"start_frame": 1095, "end_frame": 1127, "text": "彻底化解偏见和刻板印象"},
        {"start_frame": 1128, "end_frame": 1174, "text": "那也许我们都能接受彼此的差异"},
        {"start_frame": 1176, "end_frame": 1202, "text": "一起成为更好的动物"},
        {"start_frame": 1214, "end_frame": 1242, "text": "我们会侦破新案子，让世界更美好"},
        {"start_frame": 1243, "end_frame": 1264, "text": "成为史上最棒的搭档"},
        {"start_frame": 1274, "end_frame": 1289, "text": "我们又没那么不同"},
        {"start_frame": 1311, "end_frame": 1348, "text": "动物方城市2"},
        {"start_frame": 1410, "end_frame": 1472, "text": "在ZPD，搭档合作是成功的基石"},
        {"start_frame": 1473, "end_frame": 1494, "text": "想把坏人绳之以法"},
        {"start_frame": 1495, "end_frame": 1523, "text": "就要有默契，每天合作无间"},
        {"start_frame": 1525, "end_frame": 1556, "text": "今天的坏蛋是这个海关检查员"},
        {"start_frame": 1558, "end_frame": 1588, "text": "这个家伙一直在利用船坞"},
        {"start_frame": 1590, "end_frame": 1603, "text": "走私非法的海外货物"},
        {"start_frame": 1605, "end_frame": 1638, "text": "行动由霍队长和阿杜队长带领"},
        {"start_frame": 1640, "end_frame": 1662, "text": "何大马、胖胖，左翼"},
        {"start_frame": 1663, "end_frame": 1680, "text": "老却、宝龙，右翼"},
        {"start_frame": 1682, "end_frame": 1712, "text": "留守中间的是斑宝兄弟 -斑宝"},
        {"start_frame": 1713, "end_frame": 1736, "text": "哈胡组，我知道你们急着求表现"},
        {"start_frame": 1738, "end_frame": 1764, "text": "但身为菜鸟，你们只能观摩"},
        {"start_frame": 1765, "end_frame": 1802, "text": "让资深团队教你们如何…"},
        {"start_frame": 1804, "end_frame": 1810, "text": "（哈，胡）"},
    ],
    [
        {"start_frame": 1809, "end_frame": 1812, "text": "（哈，胡）"},
        {"start_frame": 1813, "end_frame": 1824, "text": "局长，嗨"},
        {"start_frame": 1826, "end_frame": 1837, "text": "要找尼克和茱蒂的话"},
        {"start_frame": 1839, "end_frame": 1857, "text": "他们说：「没问题」"},
        {"start_frame": 1859, "end_frame": 1895, "text": "他们已抵达现场，还带着他们的宝宝"},
        {"start_frame": 1953, "end_frame": 1974, "text": "（船坞）"},
        {"start_frame": 1985, "end_frame": 1996, "text": "这方法可行吗？"},
        {"start_frame": 1998, "end_frame": 2015, "text": "是你说我们得采取突袭"},
        {"start_frame": 2017, "end_frame": 2037, "text": "跟着我做好吗？自然一点"},
        {"start_frame": 2039, "end_frame": 2049, "text": "嘿 -来了"},
        {"start_frame": 2051, "end_frame": 2061, "text": "哈啰"},
        {"start_frame": 2063, "end_frame": 2077, "text": "这里是管制区"},
        {"start_frame": 2078, "end_frame": 2090, "text": "你们不能来"},
        {"start_frame": 2100, "end_frame": 2117, "text": "狐狸和兔子，好吧"},
        {"start_frame": 2118, "end_frame": 2139, "text": "没错，更是骄傲的父母"},
        {"start_frame": 2141, "end_frame": 2161, "text": "你是… -检查员"},
        {"start_frame": 2163, "end_frame": 2174, "text": "史奴利检查员"},
        {"start_frame": 2176, "end_frame": 2204, "text": "检查员？你们都做什么啊？"},
        {"start_frame": 2206, "end_frame": 2239, "text": "防止非法物品透过这些货柜走私进来？"},
        {"start_frame": 2248, "end_frame": 2268, "text": "这种问法怪怪的"},
        {"start_frame": 2270, "end_frame": 2291, "text": "反正你们得离开，不能进来"},
        {"start_frame": 2293, "end_frame": 2315, "text": "等等，等等"},
        {"start_frame": 2317, "end_frame": 2334, "text": "他说得对，真是的"},
        {"start_frame": 2340, "end_frame": 2349, "text": "你说得对，先生"},
        {"start_frame": 2357, "end_frame": 2382, "text": "我们该想清楚的，对吧，宝贝？"},
        {"start_frame": 2390, "end_frame": 2422, "text": "在我们决定来这里"},
        {"start_frame": 2423, "end_frame": 2457, "text": "到这座美丽的工业船坞"},
        {"start_frame": 2458, "end_frame": 2514, "text": "庆祝宝贝儿子生日之前就该想到的"},
        {"start_frame": 2515, "end_frame": 2542, "text": "今天他生日？"},
    ],
]

ZOOTOPIA_SCHEME_B_SINGLE_BATCH: list[dict[str, Any]] = [
    {"start_frame": 260, "end_frame": 287, "text": "我流血啦…我死啦"},
    {"start_frame": 288, "end_frame": 299, "text": "（前情提要…）"},
    {"start_frame": 300, "end_frame": 314, "text": "好啦，你也演太大了"},
    {"start_frame": 316, "end_frame": 327, "text": "可以收工了"},
    {"start_frame": 329, "end_frame": 338, "text": "（前情蹄要…）"},
    {"start_frame": 340, "end_frame": 359, "text": "可以收工啦，谢啰，长舌妇"},
    {"start_frame": 360, "end_frame": 372, "text": "你整个突破盲肠了"},
    {"start_frame": 374, "end_frame": 402, "text": "我嫁祸给狮明德，也能嫁祸给你"},
    {"start_frame": 403, "end_frame": 424, "text": "尽管说你的片面之词吧"},
    {"start_frame": 426, "end_frame": 439, "text": "事实上…"},
    {"start_frame": 440, "end_frame": 463, "text": "我会利用动物方城市所有猎食者"},
    {"start_frame": 465, "end_frame": 472, "text": "来保持现状"},
    {"start_frame": 474, "end_frame": 492, "text": "是你的片面之词"},
    {"start_frame": 499, "end_frame": 519, "text": "这叫说话的艺术，宝贝"},
    {"start_frame": 521, "end_frame": 530, "text": "砰"},
    {"start_frame": 560, "end_frame": 567, "text": "不寻常的拍档…"},
    {"start_frame": 569, "end_frame": 579, "text": "（动物方城市新闻台）"},
    {"start_frame": 581, "end_frame": 598, "text": "哈茱蒂，本市第一位兔警员"},
    {"start_frame": 600, "end_frame": 625, "text": "跟胡尼克，一位平凡的狐狸"},
    {"start_frame": 627, "end_frame": 644, "text": "（前市长杨咩咩入狱）"},
    {"start_frame": 646, "end_frame": 659, "text": "一起揭穿了杨咩咩市长的阴谋"},
    {"start_frame": 661, "end_frame": 682, "text": "用红萝卜玩具录音笔"},
    {"start_frame": 684, "end_frame": 698, "text": "我也能嫁祸给你"},
    {"start_frame": 742, "end_frame": 772, "text": "后来，胡尼克加入ZPD警局"},
    {"start_frame": 774, "end_frame": 794, "text": "成为动物方城市史上"},
    {"start_frame": 796, "end_frame": 807, "text": "（动物方城市警察学校）"},
    {"start_frame": 808, "end_frame": 824, "text": "第一组兔狐搭档"},
    {"start_frame": 838, "end_frame": 862, "text": "新任市长暨前演员马立夯"},
    {"start_frame": 864, "end_frame": 878, "text": "在动物方城市气候墙"},
    {"start_frame": 880, "end_frame": 905, "text": "发明一百周年庆祝活动上"},
    {"start_frame": 907, "end_frame": 934, "text": "表扬他们两位"},
    {"start_frame": 936, "end_frame": 958, "text": "气候墙让各种环境和动物"},
    {"start_frame": 960, "end_frame": 969, "text": "（鱼镇市场，克拉克比目鱼）"},
    {"start_frame": 971, "end_frame": 982, "text": "能在一个城市中共存"},
    {"start_frame": 983, "end_frame": 1016, "text": "这是我们建城百年纪念"},
    {"start_frame": 1018, "end_frame": 1038, "text": "如果出身卑微的乡下兔子"},
    {"start_frame": 1040, "end_frame": 1064, "text": "跟可能有案底的奸诈狐狸"},
    {"start_frame": 1066, "end_frame": 1093, "text": "能不顾他们惊人的差异"},
    {"start_frame": 1095, "end_frame": 1127, "text": "彻底化解偏见和刻板印象"},
    {"start_frame": 1128, "end_frame": 1174, "text": "那也许我们都能接受彼此的差异"},
    {"start_frame": 1176, "end_frame": 1202, "text": "一起成为更好的动物"},
    {"start_frame": 1214, "end_frame": 1242, "text": "我们会侦破新案子，让世界更美好"},
    {"start_frame": 1243, "end_frame": 1264, "text": "成为史上最棒的搭档"},
    {"start_frame": 1274, "end_frame": 1289, "text": "我们又没那么不同"},
    {"start_frame": 1311, "end_frame": 1348, "text": "动物方城市2"},
    {"start_frame": 1410, "end_frame": 1472, "text": "在ZPD，搭档合作是成功的基石"},
    {"start_frame": 1473, "end_frame": 1494, "text": "想把坏人绳之以法"},
    {"start_frame": 1495, "end_frame": 1523, "text": "就要有默契，每天合作无间"},
    {"start_frame": 1525, "end_frame": 1556, "text": "今天的坏蛋是这个海关检查员"},
    {"start_frame": 1558, "end_frame": 1588, "text": "这个家伙一直在利用船坞"},
    {"start_frame": 1590, "end_frame": 1603, "text": "走私非法的海外货物"},
    {"start_frame": 1605, "end_frame": 1638, "text": "行动由霍队长和阿杜队长带领"},
    {"start_frame": 1640, "end_frame": 1662, "text": "何大马、胖胖，左翼"},
    {"start_frame": 1663, "end_frame": 1680, "text": "老却、宝龙，右翼"},
    {"start_frame": 1682, "end_frame": 1712, "text": "留守中间的是斑宝兄弟 -斑宝"},
    {"start_frame": 1713, "end_frame": 1736, "text": "哈胡组，我知道你们急着求表现"},
    {"start_frame": 1738, "end_frame": 1764, "text": "但身为菜鸟，你们只能观摩"},
    {"start_frame": 1765, "end_frame": 1802, "text": "让资深团队教你们如何…"},
    {"start_frame": 1804, "end_frame": 1812, "text": "（哈，胡）"},
    {"start_frame": 1813, "end_frame": 1824, "text": "局长，嗨"},
    {"start_frame": 1826, "end_frame": 1837, "text": "要找尼克和茱蒂的话"},
    {"start_frame": 1839, "end_frame": 1857, "text": "他们说：「没问题」"},
    {"start_frame": 1859, "end_frame": 1895, "text": "他们已抵达现场，还带着他们的宝宝"},
    {"start_frame": 1953, "end_frame": 1974, "text": "（船坞）"},
    {"start_frame": 1985, "end_frame": 1996, "text": "这方法可行吗？"},
    {"start_frame": 1998, "end_frame": 2015, "text": "是你说我们得采取突袭"},
    {"start_frame": 2017, "end_frame": 2037, "text": "跟着我做好吗？自然一点"},
    {"start_frame": 2039, "end_frame": 2049, "text": "嘿 -来了"},
    {"start_frame": 2051, "end_frame": 2061, "text": "哈啰"},
    {"start_frame": 2063, "end_frame": 2077, "text": "这里是管制区"},
    {"start_frame": 2078, "end_frame": 2090, "text": "你们不能来"},
    {"start_frame": 2100, "end_frame": 2117, "text": "狐狸和兔子，好吧"},
    {"start_frame": 2118, "end_frame": 2139, "text": "没错，更是骄傲的父母"},
    {"start_frame": 2141, "end_frame": 2161, "text": "你是… -检查员"},
    {"start_frame": 2163, "end_frame": 2174, "text": "史奴利检查员"},
    {"start_frame": 2176, "end_frame": 2204, "text": "检查员？你们都做什么啊？"},
    {"start_frame": 2206, "end_frame": 2239, "text": "防止非法物品透过这些货柜走私进来？"},
    {"start_frame": 2248, "end_frame": 2268, "text": "这种问法怪怪的"},
    {"start_frame": 2270, "end_frame": 2291, "text": "反正你们得离开，不能进来"},
    {"start_frame": 2293, "end_frame": 2315, "text": "等等，等等"},
    {"start_frame": 2317, "end_frame": 2334, "text": "他说得对，真是的"},
    {"start_frame": 2340, "end_frame": 2349, "text": "你说得对，先生"},
    {"start_frame": 2357, "end_frame": 2382, "text": "我们该想清楚的，对吧，宝贝？"},
    {"start_frame": 2390, "end_frame": 2422, "text": "在我们决定来这里"},
    {"start_frame": 2423, "end_frame": 2457, "text": "到这座美丽的工业船坞"},
    {"start_frame": 2458, "end_frame": 2514, "text": "庆祝宝贝儿子生日之前就该想到的"},
    {"start_frame": 2515, "end_frame": 2542, "text": "今天他生日？"},
]


def norm_punct(text: str) -> str:
    return text.replace("(", "（").replace(")", "）").strip()


def stitch_batches(batches: list[list[dict[str, Any]]], max_overlap_gap: int = 2) -> list[dict[str, Any]]:
    merged: list[dict[str, Any]] = []
    for batch in batches:
        for item in batch:
            if item.get("category", "dialogue") not in ("dialogue", "bracket_note"):
                continue
            it = {
                "start_frame": int(item["start_frame"]),
                "end_frame": int(item["end_frame"]),
                "text": norm_punct(str(item["text"])),
            }
            if (
                merged
                and merged[-1]["text"] == it["text"]
                and it["start_frame"] <= merged[-1]["end_frame"] + max_overlap_gap
            ):
                merged[-1]["end_frame"] = max(merged[-1]["end_frame"], it["end_frame"])
            else:
                merged.append(it)
    return merged


def frames_to_cues(items: list[dict[str, Any]], fps: int = 10) -> list[dict[str, Any]]:
    step_ms = 1000.0 / fps
    out: list[dict[str, Any]] = []
    for it in items:
        out.append({
            "start_ms": int(round(it["start_frame"] * step_ms)),
            "end_ms": int(round((it["end_frame"] + 1) * step_ms)),
            "start_frame": it["start_frame"],
            "end_frame": it["end_frame"],
            "text": norm_punct(it["text"]),
        })
    return out


def format_srt_ts(ms: int) -> str:
    h, rem = divmod(ms, 3600000)
    m, rem = divmod(rem, 60000)
    s, millis = divmod(rem, 1000)
    return f"{h:02d}:{m:02d}:{s:02d},{millis:03d}"


def write_srt(cues: list[dict[str, Any]], path: Path) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    lines: list[str] = []
    for i, c in enumerate(cues, 1):
        lines.append(f"{i}")
        lines.append(f"{format_srt_ts(c['start_ms'])} --> {format_srt_ts(c['end_ms'])}")
        lines.append(c["text"])
        lines.append("")
    path.write_text("\n".join(lines), encoding="utf-8")


def parse_srt(path: Path) -> list[dict[str, Any]]:
    content = path.read_text(encoding="utf-8")
    blocks = re.split(r"\n\s*\n", content.strip())
    cues: list[dict[str, Any]] = []
    for b in blocks:
        lines = [ln.strip() for ln in b.strip().splitlines() if ln.strip()]
        if len(lines) < 2:
            continue
        m = re.match(
            r"(\d+):(\d+):(\d+)[,.](\d+)\s*-->\s*(\d+):(\d+):(\d+)[,.](\d+)",
            lines[1],
        )
        if not m:
            continue
        g = [int(x) for x in m.groups()]
        start_ms = ((g[0] * 60 + g[1]) * 60 + g[2]) * 1000 + g[3]
        end_ms = ((g[4] * 60 + g[5]) * 60 + g[6]) * 1000 + g[7]
        text = " ".join(lines[2:]) if len(lines) >= 3 else ""
        cues.append({"start_ms": start_ms, "end_ms": end_ms, "text": text})
    return cues


def evaluate_against_gt(pred_cues: list[dict[str, Any]], gt_cues: list[dict[str, Any]]) -> dict[str, Any]:
    exact_text = sum(1 for g, p in zip(gt_cues, pred_cues) if g["text"] == p["text"])
    start_errs = [abs(p["start_ms"] - g["start_ms"]) for g, p in zip(gt_cues, pred_cues)]
    end_errs = [abs(p["end_ms"] - g["end_ms"]) for g, p in zip(gt_cues, pred_cues)]
    return {
        "pred_count": len(pred_cues),
        "gt_count": len(gt_cues),
        "exact_text_matches": exact_text,
        "exact_match_rate": round(exact_text / max(1, len(gt_cues)), 4),
        "start_mae_ms": round(sum(start_errs) / max(1, len(start_errs)), 2),
        "start_max_ms": max(start_errs) if start_errs else 0,
        "end_mae_ms": round(sum(end_errs) / max(1, len(end_errs)), 2),
        "end_max_ms": max(end_errs) if end_errs else 0,
    }


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Stitch multimodal JSON frame batches into SRT and evaluate.")
    parser.add_argument("--inputs", nargs="*", type=Path, help="Input JSON batch files.")
    parser.add_argument("--out-srt", type=Path, help="Output SRT file path.")
    parser.add_argument("--gt-srt", type=Path, help="Optional reference GT SRT file for comparison.")
    parser.add_argument("--fps", type=int, default=10, help="Frame rate used in filmstrips (default: 10).")
    parser.add_argument(
        "--reproduce-zootopia-ab",
        action="store_true",
        help="Replay the Zootopia_clip_1080p.mp4 Scheme A vs Scheme B blind experiment.",
    )
    parser.add_argument(
        "--out-dir",
        type=Path,
        default=Path("/Volumes/lab/pp/SubLift/debug/gt_experiment"),
        help="Output directory when running --reproduce-zootopia-ab.",
    )
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    if args.reproduce_zootopia_ab:
        scheme_a = stitch_batches(ZOOTOPIA_SCHEME_A_BATCHES)
        scheme_b = stitch_batches([ZOOTOPIA_SCHEME_B_SINGLE_BATCH])
        diffs_ab = [
            (i + 1, ca, cb)
            for i, (ca, cb) in enumerate(zip(scheme_a, scheme_b))
            if ca != cb
        ]
        cues_a = frames_to_cues(scheme_a, fps=args.fps)
        cues_b = frames_to_cues(scheme_b, fps=args.fps)
        write_srt(cues_a, args.out_dir / "scheme_a.srt")
        write_srt(cues_b, args.out_dir / "scheme_b.srt")

        gt_path = args.gt_srt or Path("/Volumes/lab/pp/SubLift/benchmark/datasets/Zootopia_clip_1080p_gt.srt")
        gt_cues = parse_srt(gt_path)
        report = {
            "scheme_a_cues": len(scheme_a),
            "scheme_b_cues": len(scheme_b),
            "scheme_a_vs_b_diffs": len(diffs_ab),
            "evaluation_vs_24fps_gt": evaluate_against_gt(cues_b, gt_cues),
        }
        print(json.dumps(report, indent=2, ensure_ascii=False))
        return

    if not args.inputs or not args.out_srt:
        raise SystemExit("Provide --inputs and --out-srt, or pass --reproduce-zootopia-ab.")

    batches = [json.loads(p.read_text(encoding="utf-8")) for p in args.inputs]
    stitched = stitch_batches(batches)
    cues = frames_to_cues(stitched, fps=args.fps)
    write_srt(cues, args.out_srt)
    print(f"Wrote {len(cues)} stitched cues to {args.out_srt}")

    if args.gt_srt and args.gt_srt.exists():
        gt_cues = parse_srt(args.gt_srt)
        print(json.dumps(evaluate_against_gt(cues, gt_cues), indent=2, ensure_ascii=False))


if __name__ == "__main__":
    main()
