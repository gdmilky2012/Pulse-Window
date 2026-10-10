# Pulse Window
Frame window trainer untuk Geometry Dash (Geode).
- Lingkaran berisi jumlah frame durasi input (hijau <= batas, merah > batas).
- Counter kanan atas: "Frame N: jumlah" + total input.
- Ship / Wave / Robot: hitung dari tekan sampai lepas. Mode lain: hanya hitung input (tekan).
- Suara klik saat input dimulai.
- Impor macro (tombol "Impor Macro" di menu utama dan menu pause): membaca file macro dan
  menghitung histogram panjang hold. Tidak memutar ulang macro.
  Format: GDR2 (.gdr2/.gdr), GDR1 (.gdr, JSON/MessagePack), Silicate 2 & 3 (.slc),
  Mega Hack (.mhr biner dan .mhr.json), zBot Frame (.zbf), xdBot teks (.xd), Plaintext (.txt).
Build: upload ke GitHub, jalankan workflow "Build Pulse Window", ambil .geode dari Artifacts.
