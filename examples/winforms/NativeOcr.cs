using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Drawing;
using System.Drawing.Imaging;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using System.Web.Script.Serialization;
using Microsoft.Win32.SafeHandles;

namespace Lw.PPOCR.VulkanDemo {
    [StructLayout(LayoutKind.Sequential)]
    internal struct OcrConfig {
        public uint struct_size,device_index,det_limit_side,max_candidates,enable_classifier,use_dilation,reading_order,reserved;
        public ulong max_workspace_bytes,max_crop_pixels,max_total_crop_pixels;
        public float bitmap_threshold,box_threshold,unclip_ratio,cls_threshold;
    }
    [StructLayout(LayoutKind.Sequential)]
    internal struct DeviceInfo {
        public uint struct_size,device_index,vendor_id,device_id,api_version,device_type,max_shared_memory_bytes,subgroup_size;
        [MarshalAs(UnmanagedType.ByValArray,SizeConst=256)] public byte[] name;
        public override string ToString() {
            int n=Array.IndexOf(name,(byte)0); if(n<0) n=name.Length;
            return "["+device_index+"] "+Encoding.UTF8.GetString(name,0,n)+(device_type==4?" (软件 Vulkan)":"");
        }
    }
    internal static class Native {
        internal const string Library="lw.PPOCR.Vulkan.dll";
        [DllImport(Library,CallingConvention=CallingConvention.Cdecl)] internal static extern IntPtr lwvk_version();
        [DllImport(Library,CallingConvention=CallingConvention.Cdecl)] internal static extern IntPtr lwvk_last_error();
        [DllImport(Library,CallingConvention=CallingConvention.Cdecl)] internal static extern int lwvk_device_count(out uint count);
        [DllImport(Library,CallingConvention=CallingConvention.Cdecl)] internal static extern int lwvk_device_get(uint index,ref DeviceInfo info);
        [DllImport(Library,CallingConvention=CallingConvention.Cdecl)] internal static extern int lwvk_ocr_config_default(out OcrConfig config);
        [DllImport(Library,CallingConvention=CallingConvention.Cdecl)] internal static extern int lwvk_ocr_create(byte[] root,ref OcrConfig config,out IntPtr handle);
        [DllImport(Library,CallingConvention=CallingConvention.Cdecl)] internal static extern void lwvk_ocr_destroy(IntPtr handle);
        [DllImport(Library,CallingConvention=CallingConvention.Cdecl)] internal static extern int lwvk_ocr_run_bgr(OcrHandle handle,
            [In] byte[] pixels,ulong bytes,uint width,uint height,uint stride,out IntPtr result);
        [DllImport(Library,CallingConvention=CallingConvention.Cdecl)] internal static extern int lwvk_ocr_result_json(ResultHandle result,
            [Out] byte[] json,ulong capacity,out ulong required);
        [DllImport(Library,CallingConvention=CallingConvention.Cdecl)] internal static extern void lwvk_ocr_result_destroy(IntPtr result);
        [DllImport(Library,CallingConvention=CallingConvention.Cdecl)] internal static extern int lwvk_network_create(byte[] path,uint device,ulong workspace,out IntPtr handle);
        [DllImport(Library,CallingConvention=CallingConvention.Cdecl)] internal static extern void lwvk_network_destroy(IntPtr handle);
        [DllImport(Library,CallingConvention=CallingConvention.Cdecl)] internal static extern int lwvk_recognize_bgr(NetworkHandle handle,
            [In] byte[] pixels,ulong bytes,uint width,uint height,uint stride,uint recWidth,
            [Out] byte[] text,ulong capacity,out ulong required,out float score,out double ms);
        internal static string Utf8(IntPtr p) {
            if(p==IntPtr.Zero) return "";
            int n=0; while(n<1048576 && Marshal.ReadByte(p,n)!=0) ++n;
            byte[] bytes=new byte[n]; Marshal.Copy(p,bytes,0,n); return Encoding.UTF8.GetString(bytes);
        }
        internal static void Check(int status) {
            if(status!=0) throw new InvalidOperationException("Vulkan OCR error "+status+": "+Utf8(lwvk_last_error()));
        }
        internal static byte[] PathBytes(string path) { return Encoding.UTF8.GetBytes(Path.GetFullPath(path)+"\0"); }
        internal static List<DeviceInfo> Devices() {
            uint count; Check(lwvk_device_count(out count)); var result=new List<DeviceInfo>();
            for(uint i=0;i<count;++i) {
                var info=new DeviceInfo(); info.struct_size=(uint)Marshal.SizeOf(typeof(DeviceInfo)); info.name=new byte[256];
                Check(lwvk_device_get(i,ref info)); result.Add(info);
            }
            return result;
        }
    }
    internal sealed class OcrHandle:SafeHandleZeroOrMinusOneIsInvalid {
        internal OcrHandle(IntPtr p):base(true) {SetHandle(p);}
        protected override bool ReleaseHandle() {Native.lwvk_ocr_destroy(handle);return true;}
    }
    internal sealed class NetworkHandle:SafeHandleZeroOrMinusOneIsInvalid {
        internal NetworkHandle(IntPtr p):base(true) {SetHandle(p);}
        protected override bool ReleaseHandle() {Native.lwvk_network_destroy(handle);return true;}
    }
    internal sealed class ResultHandle:SafeHandleZeroOrMinusOneIsInvalid {
        internal ResultHandle(IntPtr p):base(true) {SetHandle(p);}
        protected override bool ReleaseHandle() {Native.lwvk_ocr_result_destroy(handle);return true;}
    }
    public sealed class OcrItem {
        public double x1 {get;set;} public double y1 {get;set;}
        public double x2 {get;set;} public double y2 {get;set;}
        public double x3 {get;set;} public double y3 {get;set;}
        public double x4 {get;set;} public double y4 {get;set;}
        public string text {get;set;} public double score {get;set;} public double det_score {get;set;}
        public int cls_label {get;set;} public double cls_score {get;set;}
    }
    public sealed class OcrTiming {
        public double det_ms {get;set;} public double cls_ms {get;set;}
        public double rec_ms {get;set;} public double total_ms {get;set;}
    }
    public sealed class OcrOutput {
        public List<OcrItem> items {get;set;} public OcrTiming timing {get;set;}
        public int image_width {get;set;} public int image_height {get;set;}
        public int det_width {get;set;} public int det_height {get;set;}
        public bool classifier_enabled {get;set;}
    }
    internal sealed class FullResult {
        internal OcrOutput Output; internal string Json;
        internal double NativeCallMilliseconds,ClientMilliseconds;
        internal bool FirstCall;
    }
    internal sealed class RecResult { internal string Text; internal float Score; internal double Milliseconds; }
    internal static class ImagePixels {
        internal static Bitmap Load(string path) {
            // Decode in a using block: the displayed image doesn't lock the customer's file.
            using(var source=new Bitmap(path)) {
                Validate(source.Width,source.Height);
                var result=new Bitmap(source.Width,source.Height,PixelFormat.Format24bppRgb);
                try {
                    using(var g=Graphics.FromImage(result)) g.DrawImage(source,new Rectangle(0,0,result.Width,result.Height),
                        new Rectangle(0,0,source.Width,source.Height),GraphicsUnit.Pixel);
                    return result;
                } catch {result.Dispose();throw;}
            }
        }
        internal static void Validate(int w,int h) {
            if(w<=0 || h<=0 || w>20000 || h>20000 || (long)w*h>40000000)
                throw new ArgumentException("图片上限：4000 万像素，单边不超过 20000。");
        }
        internal static byte[] Bgr(Bitmap source) {
            Validate(source.Width,source.Height); int stride=checked(source.Width*3);
            var pixels=new byte[checked(stride*source.Height)];
            var data=source.LockBits(new Rectangle(0,0,source.Width,source.Height),ImageLockMode.ReadOnly,PixelFormat.Format24bppRgb);
            try {
                // Positive, tightly packed images need only one interop copy.
                // Preserve the row path for padding and negative strides.
                if(data.Stride==stride) Marshal.Copy(data.Scan0,pixels,0,pixels.Length);
                else for(int y=0;y<source.Height;++y) Marshal.Copy(IntPtr.Add(data.Scan0,checked(y*data.Stride)),pixels,y*stride,stride);
            } finally {source.UnlockBits(data);}
            return pixels;
        }
    }
    internal sealed class OcrSession:IDisposable {
        private OcrHandle engine; private NetworkHandle recognizer;
        private readonly string root; private readonly OcrConfig config;
        private int completedCalls;
        internal OcrSession(string root,OcrConfig config) {
            this.root=Path.GetFullPath(root); this.config=config; IntPtr p;
            Native.Check(Native.lwvk_ocr_create(Native.PathBytes(this.root),ref config,out p)); engine=new OcrHandle(p);
        }
        internal FullResult Run(Bitmap image) {
            var clientClock=Stopwatch.StartNew();
            var pixels=ImagePixels.Bgr(image); IntPtr p;
            var nativeClock=Stopwatch.StartNew();
            Native.Check(Native.lwvk_ocr_run_bgr(engine,pixels,(ulong)pixels.Length,
                (uint)image.Width,(uint)image.Height,(uint)(image.Width*3),out p));
            nativeClock.Stop();
            using(var result=new ResultHandle(p)) {
                ulong required; int status=Native.lwvk_ocr_result_json(result,null,0,out required);
                if(status!=5) {Native.Check(status);throw new InvalidOperationException("Unexpected JSON size query");}
                if(required>4*1024*1024+1) throw new InvalidOperationException("OCR JSON exceeds output limit");
                var bytes=new byte[checked((int)required)];
                Native.Check(Native.lwvk_ocr_result_json(result,bytes,(ulong)bytes.Length,out required));
                string json=Encoding.UTF8.GetString(bytes,0,checked((int)required-1));
                var serializer=new JavaScriptSerializer {MaxJsonLength=4*1024*1024,RecursionLimit=32};
                var output=serializer.Deserialize<OcrOutput>(json);clientClock.Stop();
                return new FullResult {Json=json,Output=output,NativeCallMilliseconds=nativeClock.Elapsed.TotalMilliseconds,
                    ClientMilliseconds=clientClock.Elapsed.TotalMilliseconds,FirstCall=completedCalls++==0};
            }
        }
        internal RecResult Recognize(Bitmap crop) {
            if(recognizer==null) {
                IntPtr p; Native.Check(Native.lwvk_network_create(Native.PathBytes(Path.Combine(root,File.Exists(Path.Combine(root,"rec.onnx"))?"rec.onnx":"rec/model.json")),
                    config.device_index,config.max_workspace_bytes,out p)); recognizer=new NetworkHandle(p);
            }
            var pixels=ImagePixels.Bgr(crop); var text=new byte[4096]; ulong required; float score; double ms;
            Native.Check(Native.lwvk_recognize_bgr(recognizer,pixels,(ulong)pixels.Length,(uint)crop.Width,(uint)crop.Height,
                (uint)(crop.Width*3),0,text,(ulong)text.Length,out required,out score,out ms));
            return new RecResult {Text=Encoding.UTF8.GetString(text,0,checked((int)required-1)),Score=score,Milliseconds=ms};
        }
        public void Dispose() {
            if(recognizer!=null) {recognizer.Dispose();recognizer=null;}
            if(engine!=null) {engine.Dispose();engine=null;}
        }
    }
}
