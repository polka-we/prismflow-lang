package prism;

/**
 * Встраиваемый интерпретатор Prism.
 *
 * Использование:
 *   Prism.Result r = Prism.run("print(1 + 2)", null);
 *   textView.setText(r.ok ? r.output : r.error);
 *
 * Интерпретатор рекурсивен, поэтому run() выполняется в отдельном потоке с большим стеком (16 МБ).
 * Вызывайте run() не из главного потока, если программа может работать долго.
 */
public final class Prism {
    static { System.loadLibrary("prism"); }

    public static final class Result {
        public final boolean ok;
        public final String output; // всё, что напечатал print
        public final String error;  // текст ошибки по-русски, пусто при успехе
        Result(String out, String err) { this.output = out; this.error = err; this.ok = err.isEmpty(); }
    }

    private static native String[] nativeRun(String source, String input);
    private static native String nativeVersion();

    public static String version() { return nativeVersion(); }

    /** input - строки для request.line()/request.lines(), разделённые \n (можно null). */
    public static Result run(final String source, final String input) {
        final String[][] box = new String[1][];
        Thread t = new Thread(null, new Runnable() {
            @Override public void run() { box[0] = nativeRun(source, input); }
        }, "prism", 16L * 1024 * 1024);
        t.start();
        try { t.join(); } catch (InterruptedException e) { Thread.currentThread().interrupt(); return new Result("", "прервано"); }
        return box[0] == null ? new Result("", "внутренняя ошибка") : new Result(box[0][0], box[0][1]);
    }
}
