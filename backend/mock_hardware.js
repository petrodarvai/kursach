// mock_hardware.js — Симулятор нашої годівниці Wemos D1 Mini
const mqtt = require('mqtt');

// Підключаємося до того ж брокера, що й сервер
const client = mqtt.connect('mqtt://broker.hivemq.com');

const topic_status = "lp_feeder/feeder_1/status";
const topic_command = "lp_feeder/feeder_1/command";

let currentWeight = 10; // Початкова вага в мисці (грам)
let feedLevel = 95;     // Початковий рівень корму в бункері (%)

client.on('connect', () => {
    console.log('🤖 Симулятор заліза ЗАПУЩЕНО. Віртуальний Wemos в мережі!');
    client.subscribe(topic_command);

    // Імітуємо відправку даних датчиків кожні 10 секунд
    setInterval(() => {
        const payload = {
            weight: currentWeight,
            feed_level: feedLevel,
            status: "online"
        };
        client.publish(topic_status, JSON.stringify(payload));
        console.log('📡 [Сенсори] Відправлено статус:', payload);
    }, 10000);
});

// Слухаємо команди від нашого Node.js сервера
client.on('message', (topic, message) => {
    if (topic === topic_command) {
        const cmd = JSON.parse(message.toString());
        console.log(`📥 [MQTT] Отримано команду від сервера:`, cmd);

        if (cmd.action === "feed") {
            console.log(`⚙️ [Мотор] Кроковий двигун обертається... Насипаємо ${cmd.amount}г корму.`);
            
            // Імітуємо затримку фізичного насипання (наприклад, 2 секунди)
            setTimeout(() => {
                currentWeight += parseInt(cmd.amount); // Вага в мисці збільшується
                feedLevel -= 2;                        // Корму в бункері стає трохи менше
                
                // Звітуємо серверу про успіх
                client.publish("lp_feeder/feeder_1/success", JSON.stringify({ msg: "done" }));
                console.log('✅ [Успіх] Годування завершено, звіт відправлено на сервер.');
                
                // Одразу оновлюємо статус, щоб на сайті миттєво змінилися цифри
                client.publish(topic_status, JSON.stringify({
                    weight: currentWeight,
                    feed_level: feedLevel,
                    status: "online"
                }));
            }, 2000);
        }
    }
});
