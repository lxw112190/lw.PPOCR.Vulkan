// Experimental .NET Framework 4+/modern .NET x64 C ABI example.
// Place lw.PPOCR.Vulkan.dll beside the executable; Vulkan driver is required.
using System;
using System.Runtime.InteropServices;
using System.Drawing;
using System.Drawing.Imaging;
internal static class Program {
    const string Library="lw.PPOCR.Vulkan.dll";
    [StructLayout(LayoutKind.Sequential,CharSet=CharSet.Ansi)]
    struct DeviceInfo {
        public uint struct_size,device_index,vendor_id,device_id,api_version,device_type,max_shared_memory_bytes,subgroup_size;
        [MarshalAs(UnmanagedType.ByValTStr,SizeConst=256)] public string name;
    }
    [StructLayout(LayoutKind.Sequential)]
    struct OcrConfig {
        public uint struct_size,device_index,det_limit_side,max_candidates,enable_classifier,use_dilation,reading_order,reserved;
        public ulong max_workspace_bytes,max_crop_pixels,max_total_crop_pixels;
        public float bitmap_threshold,box_threshold,unclip_ratio,cls_threshold;
    }
    [DllImport(Library,CallingConvention=CallingConvention.Cdecl)] static extern int lwvk_ocr_config_default(out OcrConfig config);
    [DllImport(Library,CallingConvention=CallingConvention.Cdecl)] static extern int lwvk_ocr_create(byte[] root,ref OcrConfig config,out IntPtr handle);
    [DllImport(Library,CallingConvention=CallingConvention.Cdecl)] static extern void lwvk_ocr_destroy(IntPtr handle);
    [DllImport(Library,CallingConvention=CallingConvention.Cdecl)] static extern int lwvk_ocr_run_bgr(IntPtr handle,
        [In] byte[] pixels,ulong bytes,uint width,uint height,uint stride,out IntPtr result);
    [DllImport(Library,CallingConvention=CallingConvention.Cdecl)] static extern int lwvk_ocr_result_json(IntPtr result,
        [Out] byte[] json,ulong capacity,out ulong required);
    [DllImport(Library,CallingConvention=CallingConvention.Cdecl)] static extern void lwvk_ocr_result_destroy(IntPtr result);
    [DllImport(Library,CallingConvention=CallingConvention.Cdecl)] static extern int lwvk_device_count(out uint count);
    [DllImport(Library,CallingConvention=CallingConvention.Cdecl)] static extern int lwvk_device_get(uint index,ref DeviceInfo info);
    [DllImport(Library,CallingConvention=CallingConvention.Cdecl)] static extern IntPtr lwvk_last_error();
    [DllImport(Library,CallingConvention=CallingConvention.Cdecl)] static extern int lwvk_detector_create(byte[] path,uint device,ulong workspace,out IntPtr handle);
    [DllImport(Library,CallingConvention=CallingConvention.Cdecl)] static extern void lwvk_detector_destroy(IntPtr handle);
    [DllImport(Library,CallingConvention=CallingConvention.Cdecl)] static extern int lwvk_detector_run(IntPtr handle,
        [In] float[] input,ulong count,uint height,uint width,[Out] float[] output,ulong capacity,out double ms);
    [DllImport(Library,CallingConvention=CallingConvention.Cdecl)] static extern int lwvk_network_create(byte[] path,uint device,ulong workspace,out IntPtr handle);
    [DllImport(Library,CallingConvention=CallingConvention.Cdecl)] static extern void lwvk_network_destroy(IntPtr handle);
    [DllImport(Library,CallingConvention=CallingConvention.Cdecl)] static extern int lwvk_recognize_bgr(IntPtr handle,
        [In] byte[] pixels,ulong bufferBytes,uint width,uint height,uint stride,uint recWidth,
        [Out] byte[] text,ulong textCapacity,out ulong required,out float score,out double ms);
    static void Check(int status) {
        if(status==0) return;
        IntPtr p=lwvk_last_error(); int n=0; while(Marshal.ReadByte(p,n)!=0) ++n;
        byte[] bytes=new byte[n]; Marshal.Copy(p,bytes,0,n);
        throw new Exception(status+": "+System.Text.Encoding.UTF8.GetString(bytes));
    }
    static int Main(string[] args) {
        try {
            uint count; Check(lwvk_device_count(out count));
            for(uint i=0;i<count;++i) {
                DeviceInfo info=new DeviceInfo(); info.struct_size=(uint)Marshal.SizeOf(typeof(DeviceInfo));
                Check(lwvk_device_get(i,ref info)); Console.WriteLine("[{0}] {1}",i,info.name);
            }
            if(args.Length>0 && args[0]=="--ocr") return FullOcr(args);
            if(args.Length>0 && args[0]=="--recognize") return Recognize(args);
            if(args.Length==0) {
                Console.WriteLine("Usage: Demo.exe model-det.json [device-index]");
                Console.WriteLine("       Demo.exe --recognize model-rec.json cropped-image.jpg [device-index] [x y width height]");
                Console.WriteLine("       Demo.exe --ocr models/ppocrv6-tiny image.jpg [device-index] [--no-cls]");
                return 0;
            }
            IntPtr handle; byte[] path=System.Text.Encoding.UTF8.GetBytes(System.IO.Path.GetFullPath(args[0])+"\0");
            uint device=args.Length>1?uint.Parse(args[1]):0;
            Check(lwvk_detector_create(path,device,0,out handle));
            try {
                // Synthetic normalized FP32 NCHW [1,3,32,32], not a camera/image decoder.
                float[] input=new float[3*32*32],output=new float[32*32]; double ms;
                Check(lwvk_detector_run(handle,input,(ulong)input.Length,32,32,output,(ulong)output.Length,out ms));
                Console.WriteLine("Tiny DET map: {0} values, {1:F3} ms. Not text recognition.",output.Length,ms);
            } finally { lwvk_detector_destroy(handle); }
            return 0;
        } catch(Exception ex) { Console.Error.WriteLine(ex.Message); return 1; }
    }
    static int FullOcr(string[] args) {
        if(args.Length<3 || args.Length>5 || (args.Length==5 && args[4]!="--no-cls"))
            throw new ArgumentException("Invalid full OCR arguments");
        OcrConfig config; Check(lwvk_ocr_config_default(out config));
        config.device_index=args.Length>3?uint.Parse(args[3]):0;
        if(args.Length==5) config.enable_classifier=0;
        IntPtr engine;
        Check(lwvk_ocr_create(System.Text.Encoding.UTF8.GetBytes(System.IO.Path.GetFullPath(args[1])+"\0"),ref config,out engine));
        try {
            using(Bitmap source=new Bitmap(args[2])) {
                if(source.Width>20000 || source.Height>20000 || (long)source.Width*source.Height>40000000)
                    throw new ArgumentException("Image exceeds 40M pixels/20000 dimension");
                using(Bitmap bitmap=new Bitmap(source.Width,source.Height,PixelFormat.Format24bppRgb)) {
                    // Explicit pixel rectangles avoid DPI-based scaling of camera/JPEG images.
                    using(Graphics g=Graphics.FromImage(bitmap)) g.DrawImage(source,
                        new Rectangle(0,0,bitmap.Width,bitmap.Height),
                        new Rectangle(0,0,source.Width,source.Height),GraphicsUnit.Pixel);
                    int stride=checked(bitmap.Width*3);
                    byte[] pixels=new byte[checked(stride*bitmap.Height)];
                    BitmapData data=bitmap.LockBits(new Rectangle(0,0,bitmap.Width,bitmap.Height),ImageLockMode.ReadOnly,PixelFormat.Format24bppRgb);
                    try {
                        for(int y=0;y<bitmap.Height;++y) Marshal.Copy(IntPtr.Add(data.Scan0,checked(y*data.Stride)),pixels,y*stride,stride);
                    } finally {bitmap.UnlockBits(data);}
                    IntPtr result;
                    Check(lwvk_ocr_run_bgr(engine,pixels,(ulong)pixels.Length,(uint)bitmap.Width,(uint)bitmap.Height,(uint)stride,out result));
                    try {
                        ulong required;
                        int status=lwvk_ocr_result_json(result,null,0,out required);
                        if(status!=5) {Check(status);throw new Exception("Unexpected result size query");}
                        byte[] json=new byte[checked((int)required)];
                        Check(lwvk_ocr_result_json(result,json,(ulong)json.Length,out required));
                        Console.OutputEncoding=System.Text.Encoding.UTF8;
                        Console.WriteLine(System.Text.Encoding.UTF8.GetString(json,0,checked((int)required-1)));
                    } finally {lwvk_ocr_result_destroy(result);}
                }
            }
            return 0;
        } finally {lwvk_ocr_destroy(engine);}
    }
    static int Recognize(string[] args) {
        if(args.Length<3 || (args.Length>4 && args.Length!=8)) throw new ArgumentException("Invalid recognition arguments");
        uint device=args.Length>3?uint.Parse(args[3]):0;
        IntPtr handle;
        Check(lwvk_network_create(System.Text.Encoding.UTF8.GetBytes(System.IO.Path.GetFullPath(args[1])+"\0"),device,0,out handle));
        try {
            using(Bitmap source=new Bitmap(args[2])) {
                Rectangle roi=args.Length==8?new Rectangle(int.Parse(args[4]),int.Parse(args[5]),int.Parse(args[6]),int.Parse(args[7])):
                    new Rectangle(0,0,source.Width,source.Height);
                if(roi.X<0 || roi.Y<0 || roi.Width<=0 || roi.Height<=0 || (long)roi.X+roi.Width>source.Width || (long)roi.Y+roi.Height>source.Height)
                    throw new ArgumentException("ROI outside image");
                using(Bitmap crop=new Bitmap(roi.Width,roi.Height,PixelFormat.Format24bppRgb)) {
                    using(Graphics g=Graphics.FromImage(crop)) g.DrawImage(source,new Rectangle(0,0,crop.Width,crop.Height),roi,GraphicsUnit.Pixel);
                    int stride=checked(crop.Width*3);
                    byte[] pixels=new byte[checked(stride*crop.Height)];
                    BitmapData data=crop.LockBits(new Rectangle(0,0,crop.Width,crop.Height),ImageLockMode.ReadOnly,PixelFormat.Format24bppRgb);
                    try {
                        // Copy row-by-row: Bitmap padding/negative stride never crosses ABI.
                        for(int y=0;y<crop.Height;++y) Marshal.Copy(IntPtr.Add(data.Scan0,checked(y*data.Stride)),pixels,y*stride,stride);
                    } finally {crop.UnlockBits(data);}
                    byte[] text=new byte[4096]; ulong required; float score; double ms;
                    Check(lwvk_recognize_bgr(handle,pixels,(ulong)pixels.Length,(uint)crop.Width,(uint)crop.Height,(uint)stride,0,
                        text,(ulong)text.Length,out required,out score,out ms));
                    Console.OutputEncoding=System.Text.Encoding.UTF8;
                    Console.WriteLine("Text: {0}",System.Text.Encoding.UTF8.GetString(text,0,checked((int)required-1)));
                    Console.WriteLine("Score: {0:F6}; GPU inference: {1:F3} ms. Recognition-only; no DET/CLS.",score,ms);
                }
            }
            return 0;
        } finally {lwvk_network_destroy(handle);}
    }
}
