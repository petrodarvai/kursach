const express = require('express');
const http = require('http');
const { Server } = require('socket.io');
const mqtt = require('mqtt');
const fs = require('fs');
const path = require('path');

const app = express();
const server = http.createServer(app);
const io = new Server(server);

const DB_FILE = path.join(__dirname, 'database.json');

// Ініціалізація бази даних у файлі
if (!fs.existsSync(DB_FILE)) {
    fs.writeFileSync(DB_FILE, JSON.stringify({ logs: [], schedules: [] }, null, 2));
}

app.use(express.static(path.join(__dirname, 'public')));

// Підключення до MQTT
const mqttClient = mqtt.connect('mqtt://broker.hivemq.com');
const topic_status = "lp_feeder/feeder_1/status";
const topic_command = "lp_feeder/feeder_1/command";

mqttClient.on('connect', () => {
    mqttClient.subscribe([topic_status, "lp_feeder/feeder_1/success", "lp_feeder/feeder_1/error"]);
});

mqttClient.on('message', (topic, message) => {
    const data = JSON.parse(message.toString());
    
    if (topic === topic_status) {
        io.emit('device_status', data);
    } else if (topic === "lp_feeder/feeder_1/success") {
        let db = JSON.parse(fs.readFileSync(DB_FILE));
        const logEntry = { time: new Date().toLocaleTimeString(), msg: "Успішне годування" };
        db.logs.unshift(logEntry);
        fs.writeFileSync(DB_FILE, JSON.stringify(db, null, 2));
        io.emit('new_log', logEntry);
    } else if (topic === "lp_feeder/feeder_1/error") {
        io.emit('device_error', data);
    }
});

// Веб-сокети для сайту
io.on('connection', (socket) => {
    let db = JSON.parse(fs.readFileSync(DB_FILE));
    socket.emit('init_data', db);

    socket.on('manual_feed', (data) => {
        mqttClient.publish(topic_command, JSON.stringify({ action: "feed", amount: parseInt(data.amount) }));
    });

    socket.on('save_schedule', (data) => {
        let db = JSON.parse(fs.readFileSync(DB_FILE));
        db.schedules = data;
        fs.writeFileSync(DB_FILE, JSON.stringify(db, null, 2));
        io.emit('update_schedules', db.schedules);
    });
});

// Перевірка розкладу кожну хвилину (Cron-імітація)
let lastCheckedMinute = -1;
setInterval(() => {
    const now = new Date();
    if (now.getSeconds() === 0 && now.getMinutes() !== lastCheckedMinute) {
        lastCheckedMinute = now.getMinutes();
        const currentTimeStr = `${String(now.getHours()).padStart(2, '0')}:${String(now.getMinutes()).padStart(2, '0')}`;
        
        let db = JSON.parse(fs.readFileSync(DB_FILE));
        db.schedules.forEach(sched => {
            if (sched.time === currentTimeStr) {
                mqttClient.publish(topic_command, JSON.stringify({ action: "feed", amount: parseInt(sched.amount) }));
            }
        });
    }
}, 1000);

server.listen(3000, () => console.log('Сервер запущено на http://localhost:3000'));
