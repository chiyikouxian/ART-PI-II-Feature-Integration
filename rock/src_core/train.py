import os, json, random, numpy as np, torch
import torch.nn as nn
from torch.utils.data import TensorDataset, DataLoader
from model import SignBLSTM

DATASET_DIR = "data/processed/final_blstm_dataset"
# 【恢复你原来的日志路径】
LOG_DIR = os.path.join("logs", "train.log")
os.makedirs(LOG_DIR, exist_ok=True)

def set_seed(seed=42):
    random.seed(seed)
    np.random.seed(seed)
    torch.manual_seed(seed)
    torch.cuda.manual_seed_all(seed)
    torch.backends.cudnn.deterministic = True
    torch.backends.cudnn.benchmark = False

def load_json(path):
    with open(path,"r",encoding="utf-8") as f: return json.load(f)

def main():
    set_seed(42)
    train_x = np.load(os.path.join(DATASET_DIR, "train", "train_data.npy"))
    train_y = np.load(os.path.join(DATASET_DIR, "train", "train_labels.npy"))
    val_x = np.load(os.path.join(DATASET_DIR, "val", "val_data.npy"))
    val_y = np.load(os.path.join(DATASET_DIR, "val", "val_labels.npy"))
    test_x = np.load(os.path.join(DATASET_DIR, "test", "test_data.npy"))
    test_y = np.load(os.path.join(DATASET_DIR, "test", "test_labels.npy"))
    label2class = load_json(os.path.join(DATASET_DIR, "meta", "label2class.json"))
    num_classes = len(label2class)

    print(f"train: {train_x.shape}, val: {val_x.shape}, test: {test_x.shape}, classes: {num_classes}")
    device = torch.device("cuda" if torch.cuda.is_available() else "cpu")
    model = SignBLSTM(input_dim=train_x.shape[-1], hidden_dim=192, num_classes=num_classes).to(device)

    xt = torch.from_numpy(train_x).float().to(device)
    yt = torch.from_numpy(train_y).long().to(device)
    xv = torch.from_numpy(val_x).float().to(device)
    yv = torch.from_numpy(val_y).long().to(device)
    xtest = torch.from_numpy(test_x).float().to(device)
    ytest = torch.from_numpy(test_y).long().to(device)

    train_loader = DataLoader(TensorDataset(xt, yt), batch_size=64, shuffle=True)
    val_loader = DataLoader(TensorDataset(xv, yv), batch_size=64)
    test_loader = DataLoader(TensorDataset(xtest, ytest), batch_size=64)

    criterion = nn.CrossEntropyLoss()
    optimizer = torch.optim.Adam(model.parameters(), lr=1e-4)
    scheduler = torch.optim.lr_scheduler.ReduceLROnPlateau(optimizer, mode='max', factor=0.5, patience=5, min_lr=1e-6)

    os.makedirs("models", exist_ok=True)
    best_ckpt = "models/signblstm_best.pth"
    last_ckpt = "models/signblstm_last.pth"
    best_val_acc = -1.0

    # 【初始化列表，保存训练过程】
    train_losses = []
    train_accs = []
    val_losses = []
    val_accs = []

    for epoch in range(1, 51):
        model.train()
        total_loss, correct = 0.0, 0
        for xb, yb in train_loader:
            optimizer.zero_grad()
            out = model(xb)
            loss = criterion(out, yb)
            loss.backward()
            optimizer.step()
            total_loss += loss.item()*yb.size(0)
            correct += (out.argmax(1)==yb).sum().item()
        train_loss = total_loss / len(train_y)
        train_acc = correct / len(train_y)

        model.eval()
        val_loss_total, val_correct = 0.0, 0
        with torch.no_grad():
            for xb, yb in val_loader:
                out = model(xb)
                val_loss_total += criterion(out, yb).item()*yb.size(0)
                val_correct += (out.argmax(1)==yb).sum().item()
        val_loss = val_loss_total / len(val_y)
        val_acc = val_correct / len(val_y)
        scheduler.step(val_acc)

        print(f"Epoch {epoch}: train_loss={train_loss:.4f} train_acc={train_acc:.4f} | val_loss={val_loss:.4f} val_acc={val_acc:.4f}")
        
        # 【保存当前 epoch 的数据】
        train_losses.append(train_loss)
        train_accs.append(train_acc)
        val_losses.append(val_loss)
        val_accs.append(val_acc)

        if val_acc > best_val_acc:
            best_val_acc = val_acc
            torch.save({"model_state_dict": model.state_dict(), "input_dim": train_x.shape[-1],
                        "hidden_dim": model.lstm.hidden_size, "num_layers": 2, "num_classes": num_classes,
                        "best_val_acc": best_val_acc}, best_ckpt)
            print(f"✅ 保存最佳模型 (val_acc={best_val_acc:.4f})")

    torch.save({"model_state_dict": model.state_dict(), "input_dim": train_x.shape[-1],
                "hidden_dim": model.lstm.hidden_size, "num_layers": 2, "num_classes": num_classes}, last_ckpt)

    # 测试评估
    best = torch.load(best_ckpt, map_location=device, weights_only=True)
    model.load_state_dict(best["model_state_dict"])
    model.eval()
    test_loss_total, test_correct = 0.0, 0
    with torch.no_grad():
        for xb, yb in test_loader:
            out = model(xb)
            test_loss_total += criterion(out, yb).item()*yb.size(0)
            test_correct += (out.argmax(1)==yb).sum().item()
    test_loss = test_loss_total / len(test_y)
    test_acc = test_correct / len(test_y)
    print(f"TEST: loss={test_loss:.4f} acc={test_acc:.4f}")

    # ==================== 【关键：保存成你原来的 JSON 格式】 ====================
    log_data = {
        "train_losses": train_losses,
        "val_losses": val_losses,
        "train_accs": train_accs,
        "val_accs": val_accs,
        "test_loss": test_loss,
        "test_acc": test_acc
    }
    
    LOG_PATH = os.path.join(LOG_DIR, "train_log.json")
    with open(LOG_PATH, "w", encoding="utf-8") as f:
        json.dump(log_data, f, indent=2)
    print(f"\n✅ 训练日志已保存到: {LOG_PATH}")
    # =========================================================================

if __name__ == "__main__":
    main()