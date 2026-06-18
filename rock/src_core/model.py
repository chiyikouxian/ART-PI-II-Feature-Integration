import torch
import torch.nn as nn


class SignBLSTM(nn.Module):
    def __init__(self, input_dim=138, hidden_dim=192, num_classes=10, num_layers=2):
        super().__init__()
        self.lstm = nn.LSTM(
            input_size=input_dim,
            hidden_size=hidden_dim,
            num_layers=num_layers,
            bidirectional=True,
            batch_first=True,
            dropout=0.3 if num_layers > 1 else 0.0
        )
        # 轻量注意力（RK3588 友好）
        self.attn_fc1 = nn.Linear(hidden_dim * 2, hidden_dim)
        self.attn_fc2 = nn.Linear(hidden_dim, 1)
        self.fc = nn.Linear(hidden_dim * 2, num_classes)

    def forward(self, x):
        lstm_out, _ = self.lstm(x)                     # (B, T, 2*H)
        attn_hidden = torch.tanh(self.attn_fc1(lstm_out))  # (B, T, H)
        attn_score = self.attn_fc2(attn_hidden).squeeze(-1) # (B, T)
        attn_weights = torch.softmax(attn_score, dim=1).unsqueeze(-1)
        out = torch.sum(lstm_out * attn_weights, dim=1) # (B, 2*H)
        out = self.fc(out)
        return out


def export_to_onnx(model, onnx_path, input_dim=138, seq_len=40):
    model.eval()
    dummy_input = torch.randn(1, seq_len, input_dim)
    torch.onnx.export(
        model,
        dummy_input,
        onnx_path,
        export_params=True,
        opset_version=12,
        do_constant_folding=True,
        input_names=['input'],
        output_names=['output'],
        dynamic_axes={'input': {0: 'batch_size'}, 'output': {0: 'batch_size'}}
    )
    print(f"✅ ONNX 模型已保存至 {onnx_path}")