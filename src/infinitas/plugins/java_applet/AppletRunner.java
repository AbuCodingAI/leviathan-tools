/**
 * Infinitas Browser - Java Applet Runner
 *
 * Loads a Java applet class, runs it headlessly, and streams rendered
 * pixel frames to stdout. Reads mouse/key events from stdin.
 *
 * Protocol:
 *   stdout → C: raw ARGB bytes, width*height*4, no header (fixed size from args)
 *   stdin  → Java: text commands:
 *     M x y        mouse move
 *     C btn x y    mouse click (btn: 1=left, 2=mid, 3=right)
 *     K keycode    key press
 */

import java.applet.*;
import java.awt.*;
import java.awt.event.*;
import java.awt.image.*;
import java.io.*;
import java.net.*;
import java.util.*;

@SuppressWarnings("deprecation")
public class AppletRunner {

    /* ── minimal AppletStub so the applet can call getParameter() etc. ── */

    static class SimpleStub implements AppletStub, AppletContext {
        private final URL codeBase;
        private final Map<String, String> params;

        SimpleStub(URL codeBase, Map<String, String> params) {
            this.codeBase = codeBase;
            this.params   = params;
        }

        public boolean isActive()           { return true; }
        public URL getDocumentBase()        { return codeBase; }
        public URL getCodeBase()            { return codeBase; }
        public String getParameter(String n){ return params.getOrDefault(n, null); }
        public AppletContext getAppletContext(){ return this; }
        public void appletResize(int w, int h){ }

        /* AppletContext stubs */
        public AudioClip getAudioClip(URL u)     { return null; }
        public Image getImage(URL u)             { return null; }
        public Applet getApplet(String name)     { return null; }
        public Enumeration<Applet> getApplets()  { return Collections.emptyEnumeration(); }
        public void showDocument(URL u)          { }
        public void showDocument(URL u, String t){ }
        public void showStatus(String s)         { }
        public InputStream getStream(String k)   { return null; }
        public Iterator<String> getStreamKeys()  { return Collections.emptyIterator(); }
        public void setStream(String k, InputStream s) { }
    }

    /* ── stdin reader thread (mouse/key events) ── */

    static Applet    g_applet;
    static Component g_component;

    static void startEventReader(final InputStream stdin) {
        Thread t = new Thread(() -> {
            BufferedReader br = new BufferedReader(new InputStreamReader(stdin));
            String line;
            try {
                while ((line = br.readLine()) != null) {
                    String[] parts = line.trim().split(" ");
                    if (parts.length < 1) continue;
                    switch (parts[0]) {
                        case "M": {
                            if (parts.length < 3) break;
                            int x = Integer.parseInt(parts[1]);
                            int y = Integer.parseInt(parts[2]);
                            g_component.dispatchEvent(new MouseEvent(
                                g_component, MouseEvent.MOUSE_MOVED,
                                System.currentTimeMillis(), 0, x, y, 0, false));
                            break;
                        }
                        case "C": {
                            if (parts.length < 4) break;
                            int btn = Integer.parseInt(parts[1]);
                            int x   = Integer.parseInt(parts[2]);
                            int y   = Integer.parseInt(parts[3]);
                            g_component.dispatchEvent(new MouseEvent(
                                g_component, MouseEvent.MOUSE_PRESSED,
                                System.currentTimeMillis(), 0, x, y, 1, false, btn));
                            g_component.dispatchEvent(new MouseEvent(
                                g_component, MouseEvent.MOUSE_RELEASED,
                                System.currentTimeMillis(), 0, x, y, 1, false, btn));
                            g_component.dispatchEvent(new MouseEvent(
                                g_component, MouseEvent.MOUSE_CLICKED,
                                System.currentTimeMillis(), 0, x, y, 1, false, btn));
                            break;
                        }
                        case "K": {
                            if (parts.length < 2) break;
                            int kc = Integer.parseInt(parts[1]);
                            g_component.dispatchEvent(new KeyEvent(
                                g_component, KeyEvent.KEY_PRESSED,
                                System.currentTimeMillis(), 0, kc,
                                KeyEvent.CHAR_UNDEFINED));
                            g_component.dispatchEvent(new KeyEvent(
                                g_component, KeyEvent.KEY_RELEASED,
                                System.currentTimeMillis(), 0, kc,
                                KeyEvent.CHAR_UNDEFINED));
                            break;
                        }
                    }
                }
            } catch (Exception e) { /* stdin closed — exit */ }
        });
        t.setDaemon(true);
        t.start();
    }

    /* ── main ── */

    public static void main(String[] args) throws Exception {
        if (args.length < 5) {
            System.err.println("Usage: AppletRunner <code> <archive> <width> <height> <codebase>");
            System.exit(1);
        }

        String code     = args[0];
        String archive  = args[1];
        int    width    = Integer.parseInt(args[2]);
        int    height   = Integer.parseInt(args[3]);
        String codebase = args[4];

        /* strip trailing .class if present */
        String className = code.endsWith(".class") ? code.substring(0, code.length() - 6) : code;
        className = className.replace('/', '.');

        /* build classloader */
        URL base = new URL(codebase);
        URL[] urls;
        if (archive != null && !archive.isEmpty()) {
            urls = new URL[]{ new URL(base, archive) };
        } else {
            urls = new URL[]{ base };
        }

        URLClassLoader loader = new URLClassLoader(urls,
            AppletRunner.class.getClassLoader());

        /* load and instantiate the applet */
        Class<?> cls    = loader.loadClass(className);
        Applet   applet = (Applet) cls.getDeclaredConstructor().newInstance();
        g_applet    = applet;
        g_component = applet;

        /* set up stub */
        Map<String, String> params = new HashMap<>();
        SimpleStub stub = new SimpleStub(base, params);
        applet.setStub(stub);
        applet.setSize(width, height);

        /* init and start */
        applet.init();
        applet.start();

        /* off-screen render surface */
        BufferedImage img = new BufferedImage(width, height,
                                               BufferedImage.TYPE_INT_ARGB);

        /* event reader */
        startEventReader(System.in);

        /* render loop → write ARGB pixels to stdout */
        DataOutputStream out = new DataOutputStream(
            new BufferedOutputStream(System.out, width * height * 4 + 64));

        int[] pixels = new int[width * height];
        byte[] bytes = new byte[width * height * 4];

        while (true) {
            Graphics2D g = img.createGraphics();
            g.setRenderingHint(RenderingHints.KEY_ANTIALIASING,
                               RenderingHints.VALUE_ANTIALIAS_ON);
            g.setColor(Color.WHITE);
            g.fillRect(0, 0, width, height);
            try { applet.update(g); } catch (Exception e) { applet.paint(g); }
            g.dispose();

            img.getRGB(0, 0, width, height, pixels, 0, width);

            /* pack as ARGB bytes */
            for (int i = 0; i < pixels.length; i++) {
                int p     = pixels[i];
                int bi    = i * 4;
                bytes[bi]     = (byte)((p >> 24) & 0xFF); /* A */
                bytes[bi + 1] = (byte)((p >> 16) & 0xFF); /* R */
                bytes[bi + 2] = (byte)((p >>  8) & 0xFF); /* G */
                bytes[bi + 3] = (byte)( p        & 0xFF); /* B */
            }
            out.write(bytes);
            out.flush();

            Thread.sleep(33); /* ~30 FPS */
        }
    }
}
