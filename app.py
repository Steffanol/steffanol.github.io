from flask import Flask, render_template, jsonify, request
from datetime import datetime
import pytz
import json
import os

app = Flask(__name__)
DATA_FILE = 'data.json'

WIB = pytz.timezone('Asia/Jakarta')

def baca_data():
    if not os.path.exists(DATA_FILE):
        data_awal = {
            "status_paket": "Tidak ada paket",
            "display_paket": "Menunggu alat...",
            "buka_servo": False,
            "riwayat": [],
            "pending_paket": False  # Flag deteksi ultrasonik belum terkonfirmasi
        }
        with open(DATA_FILE, 'w') as f:
            json.dump(data_awal, f, indent=4)
        return data_awal
    with open(DATA_FILE, 'r') as f:
        try:
            return json.load(f)
        except json.JSONDecodeError:
            return {"status_paket": "Error", "display_paket": "JSON Rusak",
                    "buka_servo": False, "riwayat": [], "pending_paket": False}

def tulis_data(data):
    with open(DATA_FILE, 'w') as f:
        json.dump(data, f, indent=4)

@app.route('/')
def index():
    return render_template('index.html')

# API 1: Melayani fetchData() di dashboard web
@app.route('/api/data', methods=['GET'])
def get_data():
    return jsonify(baca_data())

# API 2: Tombol "Buka Pintu Pos" dari web
@app.route('/api/buka-pos', methods=['POST'])
def buka_pos():
    data = baca_data()
    data["buka_servo"] = True
    tulis_data(data)
    return jsonify({"status": "success", "message": "Perintah Buka Pintu Pos Terkirim ke Alat!"})

# API 3: ESP32 lapor ultrasonik mendeteksi objek (BELUM dikonfirmasi load cell)
@app.route('/api/deteksi-ultrasonik', methods=['POST'])
def deteksi_ultrasonik():
    req_data = request.get_json()
    if req_data:
        data = baca_data()
        data["pending_paket"] = True  # Tandai ada objek terdeteksi, tunggu konfirmasi
        data["pending_jenis"] = req_data.get("jenis", "Paket")
        tulis_data(data)
        print("[SERVER] Ultrasonik mendeteksi objek. Menunggu konfirmasi load cell...")
        return jsonify({"status": "pending", "message": "Menunggu konfirmasi berat"}), 200
    return jsonify({"status": "failed"}), 400

# API 4: ESP32 lapor load cell mendeteksi berat → konfirmasi & masukkan ke riwayat
@app.route('/api/konfirmasi-loadcell', methods=['POST'])
def konfirmasi_loadcell():
    req_data = request.get_json()
    data = baca_data()

    if not data.get("pending_paket"):
        return jsonify({"status": "ignored", "message": "Tidak ada deteksi ultrasonik sebelumnya"}), 200

    berat = req_data.get("berat", 0) if req_data else 0

    if berat > 0:
        waktu_sekarang = datetime.now(WIB).strftime("%Y-%m-%d %H:%M:%S")
        jenis = data.get("pending_jenis", "Paket")

        item_baru = {
            "no": len(data["riwayat"]) + 1,
            "waktu": waktu_sekarang,
            "jenis": f"{jenis} ({berat}g)",
            "status": "Diterima"
        }

        data["status_paket"] = "Ada Paket Masuk!"
        data["display_paket"] = f"Paket {berat}g masuk pukul {waktu_sekarang.split()[1]}"
        data["riwayat"].append(item_baru)
        data["pending_paket"] = False
        data["pending_jenis"] = ""
        tulis_data(data)

        print(f"[SERVER] Paket terkonfirmasi! Berat: {berat}g. Data masuk riwayat.")
        return jsonify({"status": "success", "message": "Paket terkonfirmasi dan dicatat"}), 200
    else:
        # Load cell membaca tapi berat = 0, batalkan pending
        data["pending_paket"] = False
        data["pending_jenis"] = ""
        tulis_data(data)
        return jsonify({"status": "ignored", "message": "Berat tidak terdeteksi, pending dibatalkan"}), 200

# API 5: Polling servo dari ESP32
@app.route('/api/cek-servo', methods=['GET'])
def api_cek_servo():
    data = baca_data()
    if data.get("buka_servo"):
        data["buka_servo"] = False
        tulis_data(data)
        return jsonify({"buka": True}), 200
    return jsonify({"buka": False}), 200

if __name__ == '__main__':
    app.run(debug=True, host='0.0.0.0', port=5000)
