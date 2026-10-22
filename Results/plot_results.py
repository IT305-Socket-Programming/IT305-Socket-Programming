#!/usr/bin/env python3
# ==============================================================================
# plot_results.py - Publication-Quality Chart Generator for IT305 Part II
# Generates SVG charts with zero external dependencies (pure Python standard library)
# ==============================================================================
import csv
import sys
import os
from collections import defaultdict

def create_svg_line_chart(title, x_label, y_label, series_dict, output_path,
                          x_format="{:.2f}", y_format="{:.1f}", width=800, height=500):
    """
    Renders a publication-grade SVG chart with grid, axes, colored lines,
    markers, and a legend.
    series_dict: { series_name: { 'points': [(x, y), ...], 'color': '#hex', 'dash': '' } }
    """
    margin_left = 90
    margin_right = 160
    margin_top = 70
    margin_bottom = 70

    plot_w = width - margin_left - margin_right
    plot_h = height - margin_top - margin_bottom

    all_x = [pt[0] for s in series_dict.values() for pt in s['points']]
    all_y = [pt[1] for s in series_dict.values() for pt in s['points']]

    if not all_x or not all_y:
        return

    min_x, max_x = min(all_x), max(all_x)
    min_y = 0
    max_y = max(all_y) * 1.15 if max(all_y) > 0 else 1.0

    if min_x == max_x:
        max_x += 1.0

    def to_screen(x, y):
        sx = margin_left + ((x - min_x) / (max_x - min_x)) * plot_w
        sy = margin_top + plot_h - ((y - min_y) / (max_y - min_y)) * plot_h
        return sx, sy

    svg = []
    svg.append(f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {width} {height}" width="{width}" height="{height}">')
    svg.append('<rect width="100%" height="100%" fill="#ffffff" />')
    svg.append(f'<text x="{width/2}" y="38" font-family="system-ui, sans-serif" font-size="20" font-weight="bold" text-anchor="middle" fill="#1f2937">{title}</text>')

    # Grid & Y ticks
    num_y_ticks = 6
    for i in range(num_y_ticks + 1):
        val = min_y + (i / num_y_ticks) * (max_y - min_y)
        _, sy = to_screen(min_x, val)
        svg.append(f'<line x1="{margin_left}" y1="{sy}" x2="{margin_left + plot_w}" y2="{sy}" stroke="#e5e7eb" stroke-width="1" />')
        svg.append(f'<text x="{margin_left - 12}" y="{sy + 4}" font-family="system-ui, sans-serif" font-size="12" text-anchor="end" fill="#6b7280">{y_format.format(val)}</text>')

    # Unique sorted X ticks
    unique_x = sorted(list(set(all_x)))
    for val in unique_x:
        sx, _ = to_screen(val, min_y)
        svg.append(f'<line x1="{sx}" y1="{margin_top}" x2="{sx}" y2="{margin_top + plot_h}" stroke="#f3f4f6" stroke-width="1" stroke-dasharray="2,2" />')
        svg.append(f'<line x1="{sx}" y1="{margin_top + plot_h}" x2="{sx}" y2="{margin_top + plot_h + 6}" stroke="#9ca3af" stroke-width="1.5" />')
        svg.append(f'<text x="{sx}" y="{margin_top + plot_h + 22}" font-family="system-ui, sans-serif" font-size="12" text-anchor="middle" fill="#4b5563">{x_format.format(val)}</text>')

    # Axes
    svg.append(f'<line x1="{margin_left}" y1="{margin_top + plot_h}" x2="{margin_left + plot_w}" y2="{margin_top + plot_h}" stroke="#4b5563" stroke-width="2" />')
    svg.append(f'<line x1="{margin_left}" y1="{margin_top}" x2="{margin_left}" y2="{margin_top + plot_h}" stroke="#4b5563" stroke-width="2" />')

    # Axis Labels
    svg.append(f'<text x="{margin_left + plot_w/2}" y="{height - 20}" font-family="system-ui, sans-serif" font-size="14" font-weight="600" text-anchor="middle" fill="#374151">{x_label}</text>')
    svg.append(f'<text x="25" y="{margin_top + plot_h/2}" font-family="system-ui, sans-serif" font-size="14" font-weight="600" text-anchor="middle" fill="#374151" transform="rotate(-90, 25, {margin_top + plot_h/2})">{y_label}</text>')

    # Plot Lines and Markers
    legend_y = margin_top + 15
    for name, s in series_dict.items():
        pts = sorted(s['points'], key=lambda p: p[0])
        color = s.get('color', '#2563eb')
        dash = s.get('dash', '')
        dash_attr = f' stroke-dasharray="{dash}"' if dash else ''

        path_d = []
        screen_pts = []
        for idx, (x, y) in enumerate(pts):
            sx, sy = to_screen(x, y)
            screen_pts.append((sx, sy))
            if idx == 0:
                path_d.append(f"M {sx:.2f} {sy:.2f}")
            else:
                path_d.append(f"L {sx:.2f} {sy:.2f}")

        svg.append(f'<path d="{" ".join(path_d)}" fill="none" stroke="{color}" stroke-width="2.5"{dash_attr} />')

        # Markers
        for sx, sy in screen_pts:
            svg.append(f'<circle cx="{sx:.2f}" cy="{sy:.2f}" r="4" fill="{color}" stroke="#ffffff" stroke-width="1.5" />')

        # Legend
        lx = margin_left + plot_w + 25
        svg.append(f'<line x1="{lx}" y1="{legend_y}" x2="{lx + 24}" y2="{legend_y}" stroke="{color}" stroke-width="2.5"{dash_attr} />')
        svg.append(f'<circle cx="{lx + 12}" cy="{legend_y}" r="3.5" fill="{color}" />')
        svg.append(f'<text x="{lx + 32}" y="{legend_y + 4}" font-family="system-ui, sans-serif" font-size="12" font-weight="500" fill="#1f2937">{name}</text>')
        legend_y += 24

    svg.append('</svg>')

    with open(output_path, 'w', encoding='utf-8') as f:
        f.write('\n'.join(svg))
    print(f"Generated chart: {output_path}")

def main():
    summary_csv = sys.argv[1] if len(sys.argv) > 1 else "Results/processed/summary.csv"
    output_dir = sys.argv[2] if len(sys.argv) > 2 else "Results/graphs"
    os.makedirs(output_dir, exist_ok=True)

    rows = []
    with open(summary_csv, 'r') as f:
        reader = csv.DictReader(f)
        for r in reader:
            rows.append(r)

    # 1. Goodput vs Failure Probability
    goodput_series = {
        'Case 1: Restart': {
            'points': [(float(r['failure_prob']), float(r['avg_mbps'])) for r in rows if r['variant'] == 'Case1'],
            'color': '#ef4444'
        },
        'Case 2: Resume': {
            'points': [(float(r['failure_prob']), float(r['avg_mbps'])) for r in rows if r['variant'] == 'Case2'],
            'color': '#3b82f6'
        },
        'Case 2 Enh: 4 Streams': {
            'points': [(float(r['failure_prob']), float(r['avg_mbps'])) for r in rows if r['variant'] == 'Case2_Enhanced' and r['streams'] == '4'],
            'color': '#10b981'
        }
    }
    create_svg_line_chart(
        "Effective Goodput vs Failure Probability",
        "Failure Probability (p)",
        "Goodput (Mbps)",
        goodput_series,
        os.path.join(output_dir, "goodput_vs_failure_prob.svg")
    )

    # 2. Completion Time vs Failure Probability
    time_series = {
        'Case 1: Restart': {
            'points': [(float(r['failure_prob']), float(r['avg_elapsed_sec'])) for r in rows if r['variant'] == 'Case1'],
            'color': '#ef4444'
        },
        'Case 2: Resume': {
            'points': [(float(r['failure_prob']), float(r['avg_elapsed_sec'])) for r in rows if r['variant'] == 'Case2'],
            'color': '#3b82f6'
        },
        'Case 2 Enh: 4 Streams': {
            'points': [(float(r['failure_prob']), float(r['avg_elapsed_sec'])) for r in rows if r['variant'] == 'Case2_Enhanced' and r['streams'] == '4'],
            'color': '#10b981'
        }
    }
    create_svg_line_chart(
        "Transfer Completion Time vs Failure Probability",
        "Failure Probability (p)",
        "Completion Time (seconds)",
        time_series,
        os.path.join(output_dir, "completion_time_vs_failure_prob.svg"),
        y_format="{:.3f}"
    )

    # 3. Retransmitted Bytes vs Failure Probability
    retrans_series = {
        'Case 1: Restart': {
            'points': [(float(r['failure_prob']), float(r['avg_retransmitted_bytes']) / 1024.0) for r in rows if r['variant'] == 'Case1'],
            'color': '#ef4444'
        },
        'Case 2: Resume': {
            'points': [(float(r['failure_prob']), float(r['avg_retransmitted_bytes']) / 1024.0) for r in rows if r['variant'] == 'Case2'],
            'color': '#3b82f6'
        },
        'Case 2 Enh: 4 Streams': {
            'points': [(float(r['failure_prob']), float(r['avg_retransmitted_bytes']) / 1024.0) for r in rows if r['variant'] == 'Case2_Enhanced' and r['streams'] == '4'],
            'color': '#10b981'
        }
    }
    create_svg_line_chart(
        "Payload Retransmission vs Failure Probability",
        "Failure Probability (p)",
        "Retransmitted Payload (KB)",
        retrans_series,
        os.path.join(output_dir, "retransmitted_bytes_vs_failure_prob.svg"),
        y_format="{:.0f}"
    )

    # 4. Multi-Stream Scalability (Goodput vs K streams)
    streams_series = {
        'Zero Drop (p=0.00)': {
            'points': [(int(r['streams']), float(r['avg_mbps'])) for r in rows if r['variant'] == 'Case2_Enhanced' and r['failure_prob'] == '0.00'],
            'color': '#10b981'
        },
        'Mild Drop (p=0.05)': {
            'points': [(int(r['streams']), float(r['avg_mbps'])) for r in rows if r['variant'] == 'Case2_Enhanced' and r['failure_prob'] == '0.05'],
            'color': '#f59e0b',
            'dash': '4,4'
        }
    }
    create_svg_line_chart(
        "Multi-Stream Scalability (Case 2 Enhanced)",
        "Number of Parallel TCP Streams (K)",
        "Goodput (Mbps)",
        streams_series,
        os.path.join(output_dir, "streams_scalability.svg"),
        x_format="{:.0f}"
    )

if __name__ == '__main__':
    main()
