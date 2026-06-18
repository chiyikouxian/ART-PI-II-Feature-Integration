final_blstm_dataset 输出说明（自动生成）

该目录是 BLSTM 训练/验证/测试的最终输入数据。
数据统一格式：float32 数组，形状 (样本数, SEQ_LEN, 特征维度)。
其中 SEQ_LEN 来自 src/blstm_data_pipeline.py 里的 SEQ_LEN 配置。

目录结构（按用途分类）：
- train/：训练集
  - train_data.npy：训练数据
  - train_labels.npy：训练标签（数字）
- val/：验证集
  - val_data.npy：验证数据
  - val_labels.npy：验证标签（数字）
- test/：测试集
  - test_data.npy：测试数据
  - test_labels.npy：测试标签（数字）
- normalize/：归一化参数（z-score）
  - normalize_mean.npy：训练集均值（形状=(特征维度,)）
  - normalize_std.npy：训练集标准差（形状=(特征维度,)）
- meta/：标签映射与处理过程信息
  - class2label.json：类别字符串 -> 数字标签
  - label2class.json：数字标签 -> 类别字符串
  - sample_process_info.json：common 数据每个 sample_id 的处理信息（原始帧数、端点范围等）

训练时如何读取：
- 训练读取：train/train_data.npy + train/train_labels.npy
- 调参/早停读取：val/val_data.npy + val/val_labels.npy
- 最终评估读取：test/test_data.npy + test/test_labels.npy

提示：
- 训练/推理时如果你自己写的代码原来从根目录读取 train_data.npy，需要把路径改成 train/train_data.npy。