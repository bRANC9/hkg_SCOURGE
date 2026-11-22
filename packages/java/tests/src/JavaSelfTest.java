// SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
// SPDX-License-Identifier: AGPL-3.0-only
//
// VM self-test for the packaged headless Temurin 25 runtime.  It deliberately
// uses only standard/JDK modules that the package ships, so a missing module
// or an old-glibc/kernel incompatibility fails the VM scenario.

import com.sun.net.httpserver.HttpServer;

import java.awt.image.BufferedImage;
import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.InetSocketAddress;
import java.net.URI;
import java.net.URL;
import java.nio.ByteBuffer;
import java.nio.channels.FileChannel;
import java.nio.charset.Charset;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.StandardOpenOption;
import java.security.KeyPair;
import java.security.KeyPairGenerator;
import java.security.MessageDigest;
import java.security.SecureRandom;
import java.security.Signature;
import java.time.LocalDate;
import java.time.ZoneId;
import java.time.ZonedDateTime;
import java.time.format.DateTimeFormatter;
import java.util.HexFormat;
import java.util.Locale;
import java.util.Map;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.TimeUnit;
import java.util.jar.JarEntry;
import java.util.jar.JarOutputStream;
import java.util.zip.ZipEntry;
import java.util.zip.ZipOutputStream;
import javax.crypto.Cipher;
import javax.crypto.spec.GCMParameterSpec;
import javax.crypto.spec.SecretKeySpec;
import javax.imageio.ImageIO;
import jdk.jfr.Recording;

public final class JavaSelfTest {
    private static final String OK = "JAVA_SELFTEST_OK";

    private JavaSelfTest() {
    }

    public static void main(String[] args) throws Exception {
        String mode = args.length == 0 ? "selftest" : args[0];
        switch (mode) {
            case "selftest" -> selftest();
            case "daemon" -> daemon(args);
            case "client" -> client(args);
            default -> {
                System.err.println("unknown mode: " + mode);
                System.exit(2);
            }
        }
    }

    private static void check(boolean condition, String what) {
        if (!condition) {
            throw new IllegalStateException("FAILED: " + what);
        }
        System.out.println("ok " + what);
    }

    private static void selftest() throws Exception {
        System.setProperty("java.awt.headless", "true");
        check(Runtime.version().feature() == 25, "Runtime.version().feature() == 25");
        check(System.getProperty("java.vm.name", "").contains("VM"), "JVM report");
        check(System.getProperty("java.vendor.version", "").contains("Temurin-25"),
                "Temurin vendor version");
        System.out.println("java.version=" + System.getProperty("java.version"));
        System.out.println("java.home=" + System.getProperty("java.home"));

        MessageDigest sha = MessageDigest.getInstance("SHA-256");
        check(HexFormat.of().formatHex(sha.digest("abc".getBytes(StandardCharsets.US_ASCII)))
                        .equals("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"),
                "SHA-256 known vector");

        byte[] key = new byte[16];
        byte[] iv = new byte[12];
        SecureRandom random = new SecureRandom();
        random.nextBytes(key);
        random.nextBytes(iv);
        Cipher aes = Cipher.getInstance("AES/GCM/NoPadding");
        aes.init(Cipher.ENCRYPT_MODE, new SecretKeySpec(key, "AES"), new GCMParameterSpec(128, iv));
        byte[] plain = "scourge-java".getBytes(StandardCharsets.UTF_8);
        byte[] sealed = aes.doFinal(plain);
        aes.init(Cipher.DECRYPT_MODE, new SecretKeySpec(key, "AES"), new GCMParameterSpec(128, iv));
        check(java.util.Arrays.equals(plain, aes.doFinal(sealed)), "AES-GCM round trip");

        KeyPairGenerator ec = KeyPairGenerator.getInstance("EC");
        ec.initialize(256);
        KeyPair pair = ec.generateKeyPair();
        Signature signer = Signature.getInstance("SHA256withECDSA");
        signer.initSign(pair.getPrivate());
        signer.update(plain);
        byte[] signature = signer.sign();
        signer.initVerify(pair.getPublic());
        signer.update(plain);
        check(signer.verify(signature), "EC P-256 sign/verify");

        Charset latin9 = Charset.forName("ISO-8859-15");
        check(latin9.encode("\u20ac").get() == (byte) 0xa4, "ISO-8859-15 euro sign");

        String march = DateTimeFormatter.ofPattern("MMMM", Locale.GERMANY)
                .format(LocalDate.of(2026, 3, 19));
        check("M\u00e4rz".equals(march), "German locale month data");
        int berlinSummer = ZonedDateTime.of(2026, 7, 1, 12, 0, 0, 0,
                ZoneId.of("Europe/Berlin")).getOffset().getTotalSeconds();
        check(berlinSummer == 7200, "Europe/Berlin DST offset");

        Path tmp = Files.createTempDirectory("java-selftest-");
        try {
            Path file = tmp.resolve("data.bin");
            Files.write(file, plain, StandardOpenOption.CREATE, StandardOpenOption.WRITE);
            check(java.util.Arrays.equals(plain, Files.readAllBytes(file)), "file read/write");
            try (FileChannel channel = FileChannel.open(file, StandardOpenOption.READ)) {
                ByteBuffer mapped = channel.map(FileChannel.MapMode.READ_ONLY, 0, channel.size());
                byte[] fromMap = new byte[mapped.remaining()];
                mapped.get(fromMap);
                check(java.util.Arrays.equals(plain, fromMap), "FileChannel.map");
            }

            Path zip = tmp.resolve("test.zip");
            try (ZipOutputStream zos = new ZipOutputStream(Files.newOutputStream(zip))) {
                zos.putNextEntry(new ZipEntry("hello.txt"));
                zos.write("zipfs".getBytes(StandardCharsets.UTF_8));
                zos.closeEntry();
            }
            try (var zipfs = java.nio.file.FileSystems.newFileSystem(
                    URI.create("jar:" + zip.toUri()), Map.of())) {
                check("zipfs".equals(Files.readString(zipfs.getPath("/hello.txt"))),
                        "zipfs read");
            }

            Path jar = tmp.resolve("test.jar");
            try (JarOutputStream jos = new JarOutputStream(Files.newOutputStream(jar))) {
                jos.putNextEntry(new JarEntry("marker.txt"));
                jos.write("jar".getBytes(StandardCharsets.UTF_8));
                jos.closeEntry();
            }
            try (var jarfs = java.nio.file.FileSystems.newFileSystem(
                    URI.create("jar:" + jar.toUri()), Map.of())) {
                check("jar".equals(Files.readString(jarfs.getPath("/marker.txt"))),
                        "jar filesystem read");
            }

            BufferedImage image = new BufferedImage(64, 64, BufferedImage.TYPE_INT_RGB);
            image.setRGB(1, 1, 0x123456);
            ByteArrayOutputStream png = new ByteArrayOutputStream();
            check(ImageIO.write(image, "png", png) && png.size() > 0, "headless ImageIO PNG");
        } finally {
            try (var stream = Files.walk(tmp)) {
                stream.sorted(java.util.Comparator.reverseOrder()).forEach(path -> {
                    try {
                        Files.deleteIfExists(path);
                    } catch (IOException ignored) {
                        // best effort
                    }
                });
            }
        }

        ExecutorService pool = Executors.newFixedThreadPool(4);
        try {
            var tasks = new java.util.ArrayList<java.util.concurrent.Future<Integer>>();
            for (int i = 0; i < 8; i++) {
                final int value = i;
                tasks.add(pool.submit(() -> value * value));
            }
            int total = 0;
            for (var task : tasks) {
                total += task.get(30, TimeUnit.SECONDS);
            }
            check(total == 140, "executor / 4 threads");
        } finally {
            pool.shutdownNow();
        }

        Process process = new ProcessBuilder("/bin/true").start();
        check(process.waitFor(10, TimeUnit.SECONDS) && process.exitValue() == 0,
                "ProcessBuilder /bin/true");
        check(java.lang.management.ManagementFactory.getRuntimeMXBean()
                .getVmVersion().contains("25"), "java.management runtime bean");

        Path recordingFile = Files.createTempFile("java-selftest-", ".jfr");
        try (Recording recording = new Recording()) {
            recording.start();
            Thread.sleep(20);
            recording.stop();
            recording.dump(recordingFile);
            check(Files.size(recordingFile) > 0, "JFR recording");
        } finally {
            Files.deleteIfExists(recordingFile);
        }

        Thread.sleep(10);
        System.out.println(OK);
    }

    private static void daemon(String[] args) throws Exception {
        Path ready = null;
        Path shutdown = null;
        for (int i = 1; i < args.length; i++) {
            switch (args[i]) {
                case "--ready" -> ready = Path.of(args[++i]);
                case "--shutdown" -> shutdown = Path.of(args[++i]);
                default -> throw new IllegalArgumentException("unknown argument " + args[i]);
            }
        }
        if (ready == null || shutdown == null) {
            throw new IllegalArgumentException("daemon needs --ready and --shutdown");
        }
        final Path shutdownFile = shutdown;
        HttpServer server = HttpServer.create(new InetSocketAddress("127.0.0.1", 0), 0);
        server.createContext("/", exchange -> {
            byte[] body = "JAVA_DAEMON_OK".getBytes(StandardCharsets.UTF_8);
            exchange.sendResponseHeaders(200, body.length);
            try (OutputStream out = exchange.getResponseBody()) {
                out.write(body);
            }
        });
        server.createContext("/health", exchange -> {
            byte[] body = "ok".getBytes(StandardCharsets.UTF_8);
            exchange.sendResponseHeaders(200, body.length);
            try (OutputStream out = exchange.getResponseBody()) {
                out.write(body);
            }
        });
        server.setExecutor(Executors.newFixedThreadPool(2));
        server.start();
        int port = server.getAddress().getPort();
        Files.writeString(ready, "port=" + port + "\n", StandardCharsets.UTF_8,
                StandardOpenOption.CREATE, StandardOpenOption.TRUNCATE_EXISTING);
        Runtime.getRuntime().addShutdownHook(new Thread(() -> {
            try {
                Files.writeString(shutdownFile,
                        "shutdown=" + System.currentTimeMillis() + "\n",
                        StandardCharsets.UTF_8,
                        StandardOpenOption.CREATE, StandardOpenOption.TRUNCATE_EXISTING);
            } catch (IOException ignored) {
                // best effort
            }
            server.stop(0);
        }, "java-selftest-shutdown"));
        System.out.println("JAVA_DAEMON_READY port=" + port);
        new CountDownLatch(1).await();
    }

    private static void client(String[] args) throws Exception {
        if (args.length < 2) {
            throw new IllegalArgumentException("client needs a port");
        }
        int port = Integer.parseInt(args[1]);
        URL url = URI.create("http://127.0.0.1:" + port + "/").toURL();
        java.net.HttpURLConnection connection =
                (java.net.HttpURLConnection) url.openConnection();
        connection.setConnectTimeout(10000);
        connection.setReadTimeout(10000);
        String body;
        try (InputStream in = connection.getInputStream()) {
            body = new String(in.readAllBytes(), StandardCharsets.UTF_8);
        }
        check(connection.getResponseCode() == 200 && "JAVA_DAEMON_OK".equals(body),
                "HTTP daemon response");
        System.out.println("JAVA_CLIENT_OK");
    }
}
