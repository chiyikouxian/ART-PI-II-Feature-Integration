import openpyxl
from openpyxl.utils import get_column_letter
import sys

# Excel文件路径
EXCEL_PATH = r"C:\Users\ideapad15s\Downloads\data_raw.xlsx"

def load_excel():
    """加载Excel文件，返回工作簿和工作表"""
    try:
        wb = openpyxl.load_workbook(EXCEL_PATH)
        ws = wb.active
        return wb, ws
    except FileNotFoundError:
        print(f"错误：找不到文件 {EXCEL_PATH}")
        sys.exit(1)
    except Exception as e:
        print(f"加载Excel文件出错：{e}")
        sys.exit(1)

def get_header_columns(ws):
    """获取表头列数"""
    # 第2行（索引1）是实际表头
    header_row = list(ws.iter_rows(min_row=2, max_row=2, values_only=True))[0]
    return len(header_row)

def get_target_start_row(ws):
    """获取数据写入的起始行：固定从第10行开始，有数据则往下找第一个空行"""
    base_start_row = 10
    # 检查B列（第2列）是否为空来判断行是否可用
    for row in range(base_start_row, ws.max_row + 2):
        if ws.cell(row=row, column=2).value is None:
            return row
    return base_start_row

def parse_input_data(input_str):
    """解析输入数据，不再严格校验列数"""
    data_lines = []
    
    # 处理多条数据标记[DATA]
    if "[DATA]" in input_str:
        raw_lines = input_str.split("[DATA]")
        raw_lines = [line.strip() for line in raw_lines if line.strip()]
    else:
        raw_lines = [input_str.strip()]
    
    for line in raw_lines:
        # 移除换行符，按逗号分割
        parts = [p.strip() for p in line.replace("\n", "").split(",") if p.strip()]
        
        # 只要数据列数大于60（合理范围）就接受
        if len(parts) < 60:
            print(f"警告：数据列数过少（仅{len(parts)}列），跳过该行。")
            continue
        
        data_lines.append(parts)
    
    return data_lines

def main():
    print("=== 数据集采集工具 ===")
    
    # 加载Excel
    wb, ws = load_excel()
    total_cols = get_header_columns(ws)
    print(f"Excel表头列数：{total_cols}")
    
    # 【修改后】依次输入4个参数：采集者、速度、中文含义、动作标志
    worker = input("请输入采集者（worker）：").strip()
    speed = input("请输入速度（v）：").strip()
    label = input("请输入中文含义（label）：").strip()
    sample_id = input("请输入动作标志（sample_id）：").strip()
    
    # 确认所有参数
    print("\n--- 请确认以下信息 ---")
    print(f"采集者（worker）：{worker}")
    print(f"速度（v）：{speed}")
    print(f"中文含义（label）：{label}")
    print(f"动作标志（sample_id）：{sample_id}")
    confirm = input("确认无误？(y/n)：").strip().lower()
    if confirm != 'y':
        print("已取消，请重新运行脚本。")
        return
    
    print("\n开始数据采集。输入数据后按回车提交，输入 'q' 退出。")
    print("数据格式：timestamp_ms,hand_type,f_0_s1_acc_x,...dorsal_mag_z")
    print("多条数据格式：[DATA]数据1[DATA]数据2...\n")
    
    while True:
        # 读取用户多行输入
        print(">>> 请输入数据：")
        lines = []
        while True:
            try:
                line = input()
                if line.strip().lower() == 'q':
                    # 保存并退出
                    wb.save(EXCEL_PATH)
                    print(f"数据已保存到 {EXCEL_PATH}，程序退出。")
                    return
                lines.append(line)
            except EOFError:
                break
        
        input_str = "\n".join(lines)
        if not input_str.strip():
            continue
        
        # 解析数据
        data_rows = parse_input_data(input_str)
        if not data_rows:
            print("未解析到有效数据。")
            continue
        
        # 获取写入起始行
        start_row = get_target_start_row(ws)
        for idx, data_parts in enumerate(data_rows):
            row_num = start_row + idx
            # 【修改后】构造完整行数据：worker, v, label, sample_id, 传感器数据
            row_data = [worker, speed, label, sample_id] + data_parts
            
            # 从第2列（B列）开始写入，跳过第1列（A列）
            for col_num, value in enumerate(row_data, 2):
                # 防止列数超出Excel范围
                if col_num > total_cols:
                    break
                # 尝试转换为数字
                try:
                    if "." in value:
                        ws.cell(row=row_num, column=col_num, value=float(value))
                    else:
                        ws.cell(row=row_num, column=col_num, value=int(value))
                except (ValueError, TypeError):
                    ws.cell(row=row_num, column=col_num, value=value)
        
        # 保存
        try:
            wb.save(EXCEL_PATH)
            print(f"✅ 成功写入 {len(data_rows)} 行数据！本次写入起始行：{start_row}")
        except Exception as e:
            print(f"❌ 保存Excel文件出错：{e}")
            print("请确保文件未被其他程序打开。")

if __name__ == "__main__":
    try:
        import openpyxl
    except ImportError:
        print("错误：未安装openpyxl库。请运行 'pip install openpyxl' 安装。")
        sys.exit(1)
    
    main()