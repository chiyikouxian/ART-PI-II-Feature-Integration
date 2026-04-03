"""
数据采集行数分析脚本
功能：分析su、wei、gong三位采集者在各动作标志(GT)下的数据行数
统计所有GT的最小行数、最大行数和平均行数，并标出对应的GT名称
"""

import pandas as pd
import numpy as np
import matplotlib.pyplot as plt
import os
import warnings
warnings.filterwarnings('ignore')

def setup_chinese_font():
    """设置中文字体支持"""
    try:
        plt.rcParams['font.sans-serif'] = ['Microsoft YaHei', 'SimHei', 'WenQuanYi Zen Hei']
        plt.rcParams['axes.unicode_minus'] = False
    except Exception:
        print("警告：未找到中文字体，可能导致中文显示异常")

def load_and_clean_data(file_path):
    """
    加载并清理数据
    参数: file_path - Excel文件路径
    返回: 清理后的DataFrame
    """
    print(f"正在读取数据文件: {file_path}")
    
    try:
        df = pd.read_excel(file_path, header=1, skiprows=[0])
        
        df.columns = [str(col).strip() if pd.notna(col) else f"col_{i}" 
                      for i, col in enumerate(df.columns)]
        
        cols = []
        col_count = {}
        for col in df.columns:
            if col in col_count:
                col_count[col] += 1
                cols.append(f"{col}_{col_count[col]}")
            else:
                col_count[col] = 1
                cols.append(col)
        df.columns = cols
        
        print(f"数据读取完成：共{len(df)}行，{len(df.columns)}列")
        return df
    
    except Exception as e:
        print(f"数据读取错误: {str(e)}")
        raise

def identify_key_columns(df):
    print("\n正在识别关键列...")
    
    key_columns = {}
    
    if len(df.columns) > 1:
        collector_col = df.columns[1]
        sample_values = df[collector_col].dropna().unique()[:10]
        if any(collector in str(val).lower() for collector in ['su', 'wei', 'gong'] for val in sample_values):
            key_columns['采集者'] = collector_col
            print(f"✓ 采集者列: {collector_col}")
    
    if len(df.columns) > 4:
        gt_col = df.columns[4]
        sample_values = df[gt_col].dropna().unique()[:10]
        if any(str(val).startswith('gt_') for val in sample_values):
            key_columns['动作标志(GT)'] = gt_col
            print(f"✓ GT列: {gt_col}")
    
    required_cols = ['采集者', '动作标志(GT)']
    missing_cols = [col for col in required_cols if col not in key_columns]
    
    if missing_cols:
        print(f"✗ 缺少关键列: {', '.join(missing_cols)}")
        raise ValueError(f"数据文件缺少必要的关键列: {', '.join(missing_cols)}")
    
    return key_columns

def analyze_collector_gt_data(df, key_columns):
    print("\n正在进行数据分析...")
    
    collector_col = key_columns['采集者']
    gt_col = key_columns['动作标志(GT)']
    
    target_collectors = ['su', 'wei', 'gong']
    
    df['采集者_标准化'] = df[collector_col].astype(str).str.lower().str.strip()
    
    valid_data = df[
        (df['采集者_标准化'].isin(target_collectors)) & 
        (df[gt_col].notna())
    ].copy()
    
    if len(valid_data) == 0:
        raise ValueError("未找到目标采集者（su, wei, gong）的有效数据")
    
    print(f"有效分析数据: {len(valid_data)} 行")
    
    gt_collector_stats = valid_data.groupby(['采集者_标准化', gt_col]).size().reset_index(name='行数')
    gt_collector_stats.columns = ['采集者', '动作标志(GT)', '行数']
    gt_collector_stats = gt_collector_stats.sort_values(['采集者', '行数'], ascending=[True, False])
    
    all_gt_counts = valid_data.groupby(gt_col).size()

    # ====================== 新增：找出全局最小/最大对应的GT ======================
    global_min_gt = all_gt_counts.idxmin()
    global_max_gt = all_gt_counts.idxmax()
    global_min_val = all_gt_counts.min()
    global_max_val = all_gt_counts.max()
    
    collector_summary = []
    for collector in target_collectors:
        collector_data = valid_data[valid_data['采集者_标准化'] == collector]
        if len(collector_data) > 0:
            collector_gt_counts = collector_data.groupby(gt_col).size()
            
            min_gt = collector_gt_counts.idxmin()
            max_gt = collector_gt_counts.idxmax()
            min_val = collector_gt_counts.min()
            max_val = collector_gt_counts.max()
            
            summary = {
                '采集者': collector.upper(),
                '数据总行数': len(collector_data),
                '涉及GT数量': len(collector_gt_counts),
                '最小行数': min_val,
                '最小行数GT': min_gt,
                '最大行数': max_val,
                '最大行数GT': max_gt,
                '平均行数': round(collector_gt_counts.mean(), 2),
                '中位数行数': round(collector_gt_counts.median(), 2),
            }
            collector_summary.append(summary)
    
    overall_stats = {
        '数据总行数': len(df),
        '有效分析行数': len(valid_data),
        'GT总数': len(all_gt_counts),
        '所有GT最小行数': global_min_val,
        '所有GT最小行数GT': global_min_gt,
        '所有GT最大行数': global_max_val,
        '所有GT最大行数GT': global_max_gt,
        '所有GT平均行数': round(all_gt_counts.mean(), 2),
        '所有GT中位数行数': round(all_gt_counts.median(), 2)
    }
    
    print("数据分析完成")
    
    return {
        'gt_collector_stats': gt_collector_stats,
        'collector_summary': collector_summary,
        'all_gt_counts': all_gt_counts,
        'overall_stats': overall_stats,
        'key_columns': key_columns
    }

def generate_visualizations(results, output_dir):
    print("\n正在生成可视化图表...")
    setup_chinese_font()
    
    collector_summary = results['collector_summary']
    all_gt_counts = results['all_gt_counts']
    gt_collector_stats = results['gt_collector_stats']
    
    fig, ((ax1, ax2), (ax3, ax4)) = plt.subplots(2, 2, figsize=(16, 12))
    fig.suptitle('数据采集行数分析报告', fontsize=16, fontweight='bold')
    
    colors = ['#3498db', '#e74c3c', '#2ecc71']
    
    collectors = [item['采集者'] for item in collector_summary]
    totals = [item['数据总行数'] for item in collector_summary]
    
    bars1 = ax1.bar(collectors, totals, color=colors)
    ax1.set_title('各采集者数据总行数', fontsize=14, fontweight='bold')
    ax1.set_ylabel('数据行数')
    ax1.set_xlabel('采集者')
    for bar, total in zip(bars1, totals):
        height = bar.get_height()
        ax1.text(bar.get_x() + bar.get_width()/2., height + 50,
                f'{total:,}', ha='center', va='bottom', fontweight='bold')
    
    gt_counts = [item['涉及GT数量'] for item in collector_summary]
    bars2 = ax2.bar(collectors, gt_counts, color=colors)
    ax2.set_title('各采集者涉及的GT数量', fontsize=14, fontweight='bold')
    ax2.set_ylabel('GT数量')
    ax2.set_xlabel('采集者')
    for bar, count in zip(bars2, gt_counts):
        height = bar.get_height()
        ax2.text(bar.get_x() + bar.get_width()/2., height + 2,
                f'{count}', ha='center', va='bottom', fontweight='bold')
    
    ax3.hist(all_gt_counts.values, bins=30, color='#9b59b6', alpha=0.7, edgecolor='black')
    ax3.set_title('所有GT行数分布', fontsize=14, fontweight='bold')
    ax3.set_xlabel('行数')
    ax3.set_ylabel('GT数量')
    mean_val = all_gt_counts.mean()
    median_val = all_gt_counts.median()
    ax3.axvline(mean_val, color='red', linestyle='--', linewidth=2, 
                label=f'平均值: {mean_val:.1f}')
    ax3.axvline(median_val, color='green', linestyle='--', linewidth=2, 
                label=f'中位数: {median_val:.1f}')
    ax3.legend()
    ax3.grid(True, alpha=0.3)
    
    box_data = []
    for item in collector_summary:
        collector = item['采集者'].lower()
        collector_gt_data = gt_collector_stats[gt_collector_stats['采集者'] == collector]['行数']
        box_data.append(collector_gt_data.values)
    
    box_plot = ax4.boxplot(box_data, labels=collectors, patch_artist=True)
    for patch, color in zip(box_plot['boxes'], colors):
        patch.set_facecolor(color)
        patch.set_alpha(0.7)
    
    ax4.set_title('各采集者GT行数分布箱线图', fontsize=14, fontweight='bold')
    ax4.set_ylabel('GT行数')
    ax4.set_xlabel('采集者')
    ax4.grid(True, alpha=0.3)
    
    plt.tight_layout()
    chart_path = os.path.join(output_dir, 'gt_collector_analysis_charts.png')
    plt.savefig(chart_path, dpi=300, bbox_inches='tight')
    plt.close()
    
    print(f"图表已保存: {chart_path}")

def save_analysis_results(results, output_dir):
    print("\n正在保存分析结果...")
    
    os.makedirs(output_dir, exist_ok=True)
    
    excel_path = os.path.join(output_dir, 'gt_collector_analysis_results.xlsx')
    with pd.ExcelWriter(excel_path, engine='openpyxl') as writer:
        results['gt_collector_stats'].to_excel(writer, sheet_name='各采集者各GT统计', index=False)
        pd.DataFrame(results['collector_summary']).to_excel(writer, sheet_name='采集者汇总统计', index=False)
        
        all_gt_df = pd.DataFrame({
            '动作标志(GT)': results['all_gt_counts'].index,
            '行数': results['all_gt_counts'].values
        }).sort_values('行数', ascending=False)
        all_gt_df.to_excel(writer, sheet_name='所有GT行数统计', index=False)
        
        overall_stats_df = pd.DataFrame({
            '统计项目': list(results['overall_stats'].keys()),
            '数值': list(results['overall_stats'].values())
        })
        overall_stats_df.to_excel(writer, sheet_name='总体统计', index=False)
    
    print(f"Excel报告已保存: {excel_path}")
    
    txt_path = os.path.join(output_dir, 'gt_analysis_summary.txt')
    with open(txt_path, 'w', encoding='utf-8') as f:
        f.write("="*60 + "\n")
        f.write("         数据采集行数分析报告\n")
        f.write("="*60 + "\n\n")
        
        f.write(f"分析时间: {pd.Timestamp.now().strftime('%Y-%m-%d %H:%M:%S')}\n")
        f.write(f"数据源: {results.get('file_path', '未知')}\n\n")
        
        f.write("一、总体统计结果\n")
        f.write("-"*30 + "\n")
        for key, value in results['overall_stats'].items():
            f.write(f"{key}: {value}\n")
        
        f.write("\n二、各采集者详细统计\n")
        f.write("-"*30 + "\n")
        for item in results['collector_summary']:
            f.write(f"\n{item['采集者']}:\n")
            f.write(f"  • 数据总行数: {item['数据总行数']:,} 行\n")
            f.write(f"  • 涉及GT数量: {item['涉及GT数量']} 个\n")
            f.write(f"  • 最小行数: {item['最小行数']} 行 (GT: {item['最小行数GT']})\n")
            f.write(f"  • 最大行数: {item['最大行数']} 行 (GT: {item['最大行数GT']})\n")
            f.write(f"  • 单GT平均行数: {item['平均行数']} 行\n")
        
        f.write("\n" + "="*60)
    
    print(f"文本摘要已保存: {txt_path}")
    
    return {
        'excel_report': excel_path,
        'txt_summary': txt_path,
        'charts': 'gt_collector_analysis_charts.png'
    }

def print_analysis_summary(results):
    print("\n" + "="*80)
    print("                    数据分析结果摘要")
    print("="*80)
    
    overall = results['overall_stats']
    print("\n📊 总体统计:")
    print(f"   • 分析数据总量: {overall['有效分析行数']:,} 行")
    print(f"   • 动作标志(GT)总数: {overall['GT总数']} 个")
    print(f"   • 最小行数: {overall['所有GT最小行数']} 行  → GT = {overall['所有GT最小行数GT']}")
    print(f"   • 最大行数: {overall['所有GT最大行数']} 行  → GT = {overall['所有GT最大行数GT']}")
    print(f"   • 所有GT平均行数: {overall['所有GT平均行数']:.2f} 行")
    
    print("\n👥 各采集者表现:")
    for item in results['collector_summary']:
        print(f"\n   {item['采集者']}:")
        print(f"   • 数据量: {item['数据总行数']:,} 行")
        print(f"   • 覆盖GT: {item['涉及GT数量']} 个")
        print(f"   • 最小行数: {item['最小行数']} 行  → GT = {item['最小行数GT']}")
        print(f"   • 最大行数: {item['最大行数']} 行  → GT = {item['最大行数GT']}")
        print(f"   • 平均行数: {item['平均行数']:.2f} 行")
    
    print("\n" + "="*80)

def main():
    file_path = r"C:\Users\ideapad15s\Downloads\data_raw.xlsx"
    output_dir = os.path.dirname(file_path)
    
    print("="*80)
    print("         数据采集行数分析工具")
    print("="*80)
    
    try:
        df = load_and_clean_data(file_path)
        key_columns = identify_key_columns(df)
        results = analyze_collector_gt_data(df, key_columns)
        results['file_path'] = file_path
        
        generate_visualizations(results, output_dir)
        saved_files = save_analysis_results(results, output_dir)
        print_analysis_summary(results)
        
        print(f"\n✅ 分析完成！结果文件已保存至:")
        print(f"   • Excel详细报告: {saved_files['excel_report']}")
        print(f"   • 文本摘要: {saved_files['txt_summary']}")
        print(f"   • 可视化图表: {os.path.join(output_dir, saved_files['charts'])}")
        
    except Exception as e:
        print(f"\n❌ 分析过程出错: {str(e)}")
        print("请检查数据文件路径和格式是否正确")

if __name__ == "__main__":
    main()