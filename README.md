# xfce-net-plugin

*[English below](#english)*

---

## Español

Plugin de panel para Xfce que reemplaza a `nm-applet`. Muestra el estado de la red Wi-Fi y permite conectarse, desconectarse y gestionar redes directamente desde el panel, sin depender de herramientas externas.

### Capturas

![Panel](screenshots/panel.png)
![Popup](screenshots/popup.png)

### Características

- Comunicación directa con NetworkManager vía DBus (sin libnm)
- Soporte para múltiples adaptadores Wi-Fi
- Detección de redes seguras (WPA2/WPA3)
- Indicador de banda (2.4G / 5G / 6G)
- Conexión a redes ocultas
- Soporte VPN (WireGuard y perfiles NM)
- Sección Ethernet (visible solo si hay cable conectado)
- Switch global de Wi-Fi y switch por adaptador
- Tamaño del popup configurable desde Propiedades
- Configuración persistente en `~/.config/xfce4/panel/xfce-net-plugin.ini`
- Internacionalización: es, en, de, fr, pt_BR, it, nl, pl, ru, zh_CN, ja

### Dependencias

- GTK 3.24.38 o superior
- libxfce4panel 4.18 o superior
- NetworkManager 1.42 o superior
- gettext

### Compilación e instalación

```bash
cd xfce-net-plugin
mkdir build && cmake -S src -B build
cmake --build build
sudo cmake --install build
xfce4-panel --quit && sleep 1 && xfce4-panel &
```

### Compatibilidad

- CachyOS (rolling)
- Debian 12
- Xfce 4.18+ sobre X11

### Licencia

GPL v2. Ver [LICENSE](LICENSE).

### Créditos

Desarrollado por [Tantin1](https://github.com/Tantin1).

---

## English

<a name="english"></a>

An Xfce panel plugin that replaces `nm-applet`. Shows Wi-Fi status and lets you connect, disconnect, and manage networks directly from the panel, without relying on external tools.

### Screenshots

![Panel](screenshots/panel.png)
![Popup](screenshots/popup.png)

### Features

- Direct NetworkManager communication via DBus (no libnm)
- Multiple Wi-Fi adapter support
- Secure network detection (WPA2/WPA3)
- Band indicator (2.4G / 5G / 6G)
- Connect to hidden networks
- VPN support (WireGuard and NM profiles)
- Ethernet section (visible only when cable is connected)
- Global Wi-Fi switch and per-adapter switch
- Configurable popup size from Properties
- Persistent config at `~/.config/xfce4/panel/xfce-net-plugin.ini`
- Internationalization: es, en, de, fr, pt_BR, it, nl, pl, ru, zh_CN, ja

### Dependencies

- GTK 3.24.38 or higher
- libxfce4panel 4.18 or higher
- NetworkManager 1.42 or higher
- gettext

### Build & Install

```bash
cd xfce-net-plugin
mkdir build && cmake -S src -B build
cmake --build build
sudo cmake --install build
xfce4-panel -r
```

### Compatibility

- CachyOS (rolling)
- Debian 12
- Xfce 4.18+ on X11

### License

GPL v2. See [LICENSE](LICENSE).

### Credits

Developed by [Tantin1](https://github.com/Tantin1).
