/**
 * 主JavaScript文件
 */

// 全局配置
const CONFIG = {
    apiBaseUrl: '/api',
    refreshInterval: 1000
};

// API请求封装
const api = {
    /**
     * 发送POST请求
     */
    post: async function(url, data) {
        const response = await fetch(CONFIG.apiBaseUrl + url, {
            method: 'POST',
            headers: {
                'Content-Type': 'application/json'
            },
            body: JSON.stringify(data)
        });
        return response.json();
    },

    /**
     * 发送GET请求
     */
    get: async function(url, params = {}) {
        const queryString = new URLSearchParams(params).toString();
        const fullUrl = CONFIG.apiBaseUrl + url + (queryString ? '?' + queryString : '');
        const response = await fetch(fullUrl);
        return response.json();
    }
};

// 手套数据管理
const gloveManager = {
    isCollecting: false,

    /**
     * 开始数据采集
     */
    startCollection: function() {
        this.isCollecting = true;
        console.log('开始数据采集');
        // TODO: 实现WebSocket连接或轮询
    },

    /**
     * 停止数据采集
     */
    stopCollection: function() {
        this.isCollecting = false;
        console.log('停止数据采集');
    },

    /**
     * 获取历史数据
     */
    getHistory: async function(page = 1, perPage = 20) {
        return await api.get('/glove/data', { page, per_page: perPage });
    },

    /**
     * 识别手势
     */
    recognize: async function(sensorData) {
        return await api.post('/glove/recognize', { sensor_data: sensorData });
    }
};

// 页面初始化
document.addEventListener('DOMContentLoaded', function() {
    console.log('应用已初始化');

    // 初始化Layui组件（如果存在）
    if (typeof layui !== 'undefined') {
        layui.use(['element', 'layer', 'form'], function() {
            const element = layui.element;
            const layer = layui.layer;
            const form = layui.form;
        });
    }
});

// 工具函数
const utils = {
    /**
     * 格式化时间
     */
    formatTime: function(timestamp) {
        const date = new Date(timestamp);
        return date.toLocaleString('zh-CN');
    },

    /**
     * 显示消息提示
     */
    showMessage: function(msg, type = 'info') {
        if (typeof layer !== 'undefined') {
            layer.msg(msg, { icon: type === 'success' ? 1 : type === 'error' ? 2 : 0 });
        } else {
            alert(msg);
        }
    }
};
