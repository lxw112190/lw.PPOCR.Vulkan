using System;
using System.IO;
using System.Runtime.InteropServices;
using System.Windows.Forms;
namespace Lw.PPOCR.VulkanDemo {
    internal sealed class Options {
        internal bool Smoke,HostOnly; internal uint Device;
        internal int SmokeRepeat=1;
        internal string Report,Screenshot,SmokeModel="tiny",Root=AppDomain.CurrentDomain.BaseDirectory;
        internal static Options Parse(string[] args) {
            var result=new Options();
            for(int i=0;i<args.Length;++i) {
                if(args[i]=="--smoke") result.Smoke=true;
                else if(args[i]=="--smoke-host") {result.Smoke=true;result.HostOnly=true;}
                else if(args[i]=="--device" && i+1<args.Length) result.Device=uint.Parse(args[++i]);
                else if(args[i]=="--smoke-model" && i+1<args.Length) {
                    result.SmokeModel=args[++i].ToLowerInvariant();
                    if(result.SmokeModel!="tiny" && result.SmokeModel!="small" && result.SmokeModel!="medium")
                        throw new ArgumentException("smoke-model must be tiny, small or medium");
                }
                else if(args[i]=="--smoke-repeat" && i+1<args.Length) {
                    result.SmokeRepeat=int.Parse(args[++i]);
                    if(result.SmokeRepeat<1 || result.SmokeRepeat>100) throw new ArgumentException("smoke-repeat must be 1..100");
                }
                else if(args[i]=="--root" && i+1<args.Length) result.Root=Path.GetFullPath(args[++i]);
                else if(args[i]=="--report" && i+1<args.Length) result.Report=Path.GetFullPath(args[++i]);
                else if(args[i]=="--screenshot" && i+1<args.Length) result.Screenshot=Path.GetFullPath(args[++i]);
                else throw new ArgumentException("Unknown/missing argument: "+args[i]);
            }
            return result;
        }
    }
    internal static class Program {
        [DllImport("user32.dll")] private static extern bool SetProcessDPIAware();
        [STAThread] private static int Main(string[] args) {
            Options options=null;
            try {
                options=Options.Parse(args); SetProcessDPIAware();
                Application.EnableVisualStyles(); Application.SetCompatibleTextRenderingDefault(false);
                using(var form=new MainForm(options)) {Application.Run(form);return form.ExitCode;}
            } catch(Exception ex) {
                if(options!=null && options.Smoke && options.Report!=null) File.WriteAllText(options.Report,ex.ToString());
                else MessageBox.Show(ex.Message,"lw.PPOCR.Vulkan",MessageBoxButtons.OK,MessageBoxIcon.Error);
                return 1;
            }
        }
    }
}
