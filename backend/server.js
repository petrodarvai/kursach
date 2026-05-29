const express = require('express');
const http = require('http');
const { Server } = require('socket.io');
const mqtt = require('mqtt');
const path = require('path');
const sqlite3 = require('sqlite3').verbose(); // Підключаємо SQL

const app = express();
const server = http.createServer(app);
const io = new Server(server);

app.use(express.static(path.join(__dirname, 'public')));

// --- Налаштування бази даних SQL (SQLite) ---
const DB_FILE = path.join(__dirname, 'feeder.db');
const db = new sqlite3.Database(DB_FILE, (err) => {
    if (err) console.error('Помилка підключення до SQL:', err.message);
    else console.log('Успішно підключено до бази даних SQL (SQLite).');
});

// Створюємо таблиці, якщо їх ще немає
db.serialize(() => {
    // Таблиця для журналу активності
    db.run(`CREATE TABLE IF NOT EXISTS logs (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        time TEXT NOT NULL,
        msg TEXT NOT NULL
    )`);

    // Таблиця для розкладу годувань
    db.run(`CREATE TABLE IF NOT EXISTS schedules (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        time TEXT NOT NULL,
        amount INTEGER NOT NULL
    )`);
});

// --- Налаштування MQTT ---
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
    } 
    else if (topic === "lp_feeder/feeder_1/success") {
        const timeNow = new Date().toLocaleTimeString();
        const logMsg = "Успішне годування";

        // SQL ЗАПИТ: Додаємо новий лог у базу
        const stmt = db.prepare("INSERT INTO logs (time, msg) VALUES (?, ?)");
        stmt.run(timeNow, logMsg, function(err) {
            if (err) return console.error(err.message);
            io.emit('new_log', { time: timeNow, msg: logMsg });
        });
        stmt.finalize();
    } 
    else if (topic === "lp_feeder/feeder_1/error") {
        io.emit('device_error', data);
    }
});

// --- Веб-сокети для сайту ---
io.on('connection', (socket) => {
    
    // SQL ЗАПИТ: Витягуємо історію логів та розклад для ініціалізації сайту
    db.all("SELECT time, msg FROM logs ORDER BY id DESC LIMIT 50", [], (err, logRows) => {
        if (err) return;
        db.all("SELECT time, amount FROM schedules ORDER BY time ASC", [], (err, schedRows) => {
            if (err) return;
            // Відправляємо чисті дані на фронтенд
            socket.emit('init_data', { logs: logRows, schedules: schedRows });
        });
    });

    socket.on('manual_feed', (data) => {
        mqttClient.publish(topic_command, JSON.stringify({ action: "feed", amount: parseInt(data.amount) }));
    });

    socket.on('save_schedule', (data) => {
        // SQL ЗАПИТ: Оновлення розкладу. Очищаємо старий і записуємо новий
        db.serialize(() => {
            db.run("DELETE FROM schedules", [], (err) => {
                if (err) return;
                
                const stmt = db.prepare("INSERT INTO schedules (time, amount) VALUES (?, ?)");
                data.forEach(sched => {
                    stmt.run(sched.time, parseInt(sched.amount));
                });
                stmt.finalize(() => {
                    // Після успішного запису відправляємо оновлений список усім клієнтам
                    db.all("SELECT time, amount FROM schedules ORDER BY time ASC", [], (err, rows) => {
                        if (!err) io.emit('update_schedules', rows);
                    });
                });
            });
        });
    });
});

// --- Автоматичний розклад (Перевірка щохвилини) ---
let lastCheckedMinute = -1;
setInterval(() => {
    const now = new Date();
    if (now.getSeconds() === 0 && now.getMinutes() !== lastCheckedMinute) {
        lastCheckedMinute = now.getMinutes();
        const currentTimeStr = `${String(now.getHours()).padStart(2, '0')}:${String(now.getMinutes()).padStart(2, '0')}`;
        
        // SQL ЗАПИТ: Шукаємо в базі, чи є годування на цей час
        db.all("SELECT amount FROM schedules WHERE time = ?", [currentTimeStr], (err, rows) => {
            if (err) return console.error(err.message);
            
            rows.forEach(sched => {
                console.log(`⏰ [Розклад] Час настав (${currentTimeStr}). Сигнал для MQTT.`);
                mqttClient.publish(topic_command, JSON.stringify({ action: "feed", amount: sched.amount }));
            });
        });
    }
}, 1000);

server.listen(3000, () => console.log('Сервер запущено на http://localhost:3000'));
