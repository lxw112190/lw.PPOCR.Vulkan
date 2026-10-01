using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Drawing;
using System.Drawing.Imaging;
using System.IO;
using System.Text;
using System.Threading;
using System.Threading.Tasks;
using System.Web.Script.Serialization;
using System.Windows.Forms;

namespace Lw.PPOCR.VulkanDemo {
    internal sealed class MainForm:Form {
        private readonly Options options;
        private readonly ComboBox gpu=new ComboBox {DropDownStyle=ComboBoxStyle.DropDown,Width=370};
        private readonly TextBox modelRoot=new TextBox {Width=600};
        private readonly ComboBox modelVariant=new ComboBox {DropDownStyle=ComboBoxStyle.DropDownList,Width=100};
        private readonly NumericUpDown side=Number(32,960,960,0,32),bitmapThreshold=Number(0,1,.3m,2,.01m),
            boxThreshold=Number(0,1,.6m,2,.01m),unclip=Number(.1m,5,1.5m,2,.1m),clsThreshold=Number(0,1,.9m,2,.01m),
            workspace=Number(0,1024,0,0,64);
        private readonly CheckBox cls=new CheckBox {Text="方向分类 CLS",Checked=true,AutoSize=true};
        private readonly CheckBox dilation=new CheckBox {Text="膨胀",AutoSize=true};
        private readonly CheckBox drawBoxes=new CheckBox {Text="绘制检测框",Checked=true,AutoSize=true};
        private readonly Button init=Button("初始化 / 重载",128),release=Button("释放引擎",100),open=Button("选择图片",100),
            run=Button("整图 OCR",105),recognize=Button("框选仅识别",112),clearRoi=Button("清除框选",100),
            refresh=Button("刷新设备",100),browse=Button("模型目录",100),copy=Button("复制文字",100),save=Button("保存 JSON",108);
        private readonly ImageCanvas canvas=new ImageCanvas {Dock=DockStyle.Fill};
        private readonly TextBox text=new TextBox {Multiline=true,ReadOnly=true,Dock=DockStyle.Fill,ScrollBars=ScrollBars.Both};
        private readonly TextBox json=new TextBox {Multiline=true,ReadOnly=true,Dock=DockStyle.Fill,ScrollBars=ScrollBars.Both,WordWrap=false};
        private readonly TabControl tabs=new TabControl {Dock=DockStyle.Fill};
        private readonly DataGridView grid=new DataGridView {Dock=DockStyle.Fill,ReadOnly=true,AllowUserToAddRows=false,
            AllowUserToDeleteRows=false,RowHeadersVisible=false,SelectionMode=DataGridViewSelectionMode.FullRowSelect,MultiSelect=false};
        private readonly Label status=new Label {Dock=DockStyle.Fill,Text="准备就绪",AutoEllipsis=true,TextAlign=ContentAlignment.MiddleLeft};
        private readonly Label imageInfo=new Label {Dock=DockStyle.Top,Height=30,Text="在图片上按住鼠标左键拖动框选文字区域",TextAlign=ContentAlignment.MiddleLeft};
        private readonly TableLayoutPanel controls=new TableLayoutPanel {Dock=DockStyle.Top,AutoSize=true,ColumnCount=1,Padding=new Padding(8)};
        private readonly SplitContainer split=new SplitContainer {Size=new Size(1100,550),Dock=DockStyle.Fill,SplitterWidth=8,Panel1MinSize=260,Panel2MinSize=280};
        private List<DeviceInfo> devices=new List<DeviceInfo>();
        private OcrSession session; private bool busy,dirty=true,loading,closePending;
        private bool jsonDirty,gridDirty;
        private double lastUiReadyMilliseconds;
        private string lastJson="",lastText=""; private FullResult lastFull; private string gpuDisplay="";
        internal int ExitCode {get;private set;}
        private static NumericUpDown Number(decimal min,decimal max,decimal value,int places,decimal step) {
            return new NumericUpDown {Minimum=min,Maximum=max,Value=value,DecimalPlaces=places,Increment=step,Width=75};
        }
        private static Button Button(string caption,int width) {return new Button {Text=caption,Width=width,Height=32};}
        private static Label Caption(string caption) {return new Label {Text=caption,AutoSize=true,Margin=new Padding(7,8,3,4)};}
        private static FlowLayoutPanel Row() {return new FlowLayoutPanel {AutoSize=true,Dock=DockStyle.Fill,WrapContents=true,Margin=new Padding(0,2,0,2)};}
        internal MainForm(Options options) {
            this.options=options; Text="lw.PPOCR.Vulkan — WinForms 测试（天天代码码天天）";
            Font=new Font("Microsoft YaHei UI",9); ClientSize=new Size(1180,800); MinimumSize=new Size(920,660);
            AutoScaleMode=AutoScaleMode.Font; StartPosition=FormStartPosition.CenterScreen;
            if(options.Smoke) {ShowInTaskbar=false;Opacity=0;}
            modelVariant.Items.AddRange(new object[]{"Tiny","Small","Medium"});
            BuildLayout(); WireEvents(); modelVariant.SelectedIndex=0;
            Shown+=delegate {Start();}; FormClosing+=OnClosingForm;
        }
        private void BuildLayout() {
            var deviceRow=Row(); deviceRow.Controls.Add(Caption("Vulkan GPU（可直接填编号）")); deviceRow.Controls.Add(gpu);
            deviceRow.Controls.Add(refresh); deviceRow.Controls.Add(new Label {Text="PP-OCRv6 · Vulkan FP32",AutoSize=true,Margin=new Padding(12,8,0,0)});
            var modelRow=Row(); modelRow.Controls.Add(Caption("模型"));modelRow.Controls.Add(modelVariant);modelRow.Controls.Add(modelRoot);modelRow.Controls.Add(browse);
            var paramsRow=Row(); paramsRow.Controls.Add(Caption("DET 长边上限"));paramsRow.Controls.Add(side);
            paramsRow.Controls.Add(Caption("二值阈值"));paramsRow.Controls.Add(bitmapThreshold);
            paramsRow.Controls.Add(Caption("框阈值"));paramsRow.Controls.Add(boxThreshold);
            paramsRow.Controls.Add(Caption("扩框比例"));paramsRow.Controls.Add(unclip);
            cls.Margin=new Padding(15,8,0,0);paramsRow.Controls.Add(cls);paramsRow.Controls.Add(Caption("CLS 阈值"));paramsRow.Controls.Add(clsThreshold);
            dilation.Margin=new Padding(12,8,0,0);paramsRow.Controls.Add(dilation);
            var memoryRow=Row();memoryRow.Controls.Add(Caption("每模型工作区上限 MiB"));memoryRow.Controls.Add(workspace);
            memoryRow.Controls.Add(new Label {Text="0 = 默认 512 MiB；按需分配，不会一次占满。DET 长边默认保持 960。",AutoSize=true,Margin=new Padding(12,8,0,0)});
            var actionRow=Row();actionRow.Controls.Add(init);actionRow.Controls.Add(release);actionRow.Controls.Add(open);
            actionRow.Controls.Add(run);actionRow.Controls.Add(recognize);actionRow.Controls.Add(clearRoi);
            drawBoxes.Margin=new Padding(12,8,0,0);actionRow.Controls.Add(drawBoxes);actionRow.Controls.Add(copy);actionRow.Controls.Add(save);
            controls.Controls.Add(deviceRow);controls.Controls.Add(modelRow);controls.Controls.Add(paramsRow);controls.Controls.Add(memoryRow);controls.Controls.Add(actionRow);
            var pageText=new TabPage("文字结果");pageText.Controls.Add(text);tabs.TabPages.Add(pageText);
            var pageJson=new TabPage("详细 JSON");pageJson.Controls.Add(json);tabs.TabPages.Add(pageJson);
            var pageGrid=new TabPage("结果列表 / 置信度");pageGrid.Controls.Add(grid);tabs.TabPages.Add(pageGrid);
            grid.AutoSizeColumnsMode=DataGridViewAutoSizeColumnsMode.Fill;
            grid.Columns.Add("index","序号");grid.Columns[0].FillWeight=15;
            grid.Columns.Add("text","文字");grid.Columns[1].FillWeight=65;
            grid.Columns.Add("score","置信度");grid.Columns[2].FillWeight=20;
            foreach(DataGridViewColumn column in grid.Columns) column.SortMode=DataGridViewColumnSortMode.NotSortable;
            split.Panel1.Controls.Add(canvas);split.Panel1.Controls.Add(imageInfo);split.Panel2.Controls.Add(tabs);
            var footer=new TableLayoutPanel {Dock=DockStyle.Bottom,Height=48,ColumnCount=2,Padding=new Padding(8,0,8,0)};
            footer.ColumnStyles.Add(new ColumnStyle(SizeType.Percent,100));footer.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute,235));
            footer.Controls.Add(status,0,0);footer.Controls.Add(new Label {Text="作者：天天代码码天天\nQQ：819069052",Dock=DockStyle.Fill,TextAlign=ContentAlignment.MiddleRight},1,0);
            var body=new Panel {Dock=DockStyle.Fill,Padding=new Padding(8,0,8,0)};body.Controls.Add(split);
            Controls.Add(body);Controls.Add(footer);Controls.Add(controls);
            Resize+=delegate {if(split.Width>600) split.SplitterDistance=(int)(split.Width*.51);};
        }
        private void WireEvents() {
            modelVariant.SelectedIndexChanged+=delegate {modelRoot.Text=Path.Combine(options.Root,"models/onnx/ppocrv6-"+modelVariant.Text.ToLowerInvariant());};
            refresh.Click+=delegate {TryUi(delegate {LoadDevices();});};
            browse.Click+=delegate {using(var d=new FolderBrowserDialog()) {d.SelectedPath=modelRoot.Text;if(d.ShowDialog(this)==DialogResult.OK) modelRoot.Text=d.SelectedPath;}};
            open.Click+=delegate {using(var d=new OpenFileDialog {Filter="图片|*.jpg;*.jpeg;*.png;*.bmp;*.tif;*.tiff",CheckFileExists=true}) {
                if(d.ShowDialog(this)==DialogResult.OK) TryUi(delegate {LoadImage(d.FileName);});}};
            init.Click+=delegate {TryUi(delegate {InitializeEngine(null);});};
            release.Click+=delegate {var old=session;session=null;StartJob(delegate {if(old!=null) old.Dispose();return true;},delegate {status.Text="引擎已释放";},null);};
            run.Click+=delegate {TryUi(delegate {RunFull(null);});};
            recognize.Click+=delegate {TryUi(delegate {RunRecognition(null);});};
            clearRoi.Click+=delegate {canvas.SelectRectangle(Rectangle.Empty);};
            drawBoxes.CheckedChanged+=delegate {canvas.ShowBoxes=drawBoxes.Checked;canvas.Invalidate();};
            copy.Click+=delegate {TryUi(delegate {if(lastText.Length>0) Clipboard.SetText(lastText);});};
            save.Click+=delegate {using(var d=new SaveFileDialog {Filter="JSON|*.json",FileName="ocr-result.json"}) {
                if(d.ShowDialog(this)==DialogResult.OK) TryUi(delegate {File.WriteAllText(d.FileName,lastJson,new UTF8Encoding(false));});}};
            canvas.SelectionChanged+=delegate {UpdateImageInfo();UpdateEnabled();};
            grid.SelectionChanged+=delegate {canvas.Highlight=grid.CurrentRow==null?-1:grid.CurrentRow.Index;canvas.Invalidate();};
            tabs.SelectedIndexChanged+=delegate {RefreshDetails();};
            gpu.TextChanged+=Changed;modelRoot.TextChanged+=Changed;cls.CheckedChanged+=Changed;dilation.CheckedChanged+=Changed;
            foreach(var c in new[] {side,bitmapThreshold,boxThreshold,unclip,clsThreshold,workspace}) c.ValueChanged+=Changed;
        }
        private void Changed(object sender,EventArgs e) {
            if(loading) return;dirty=true;if(session!=null) status.Text="参数已变更，请重新初始化引擎";UpdateEnabled();
        }
        private void Start() {
            try {
                if(options.HostOnly) {
                    try {LoadDevices();} catch(InvalidOperationException ex) {status.Text="主机界面测试：跳过不可用 Vulkan 设备 · "+ex.Message;}
                } else TryUi(delegate {LoadDevices();});
                var sample=Path.Combine(options.Root,"test-images/sample.jpg"); if(File.Exists(sample)) LoadImage(sample);
                split.SplitterDistance=(int)(split.Width*.51);UpdateEnabled();
                if(options.Smoke) Smoke();
            } catch(Exception ex) {Failure(ex);}
        }
        private void LoadDevices() {
            loading=true;
            try {
                devices=Native.Devices();gpu.Items.Clear();foreach(var info in devices) gpu.Items.Add(info);
                if(devices.Count>0) {gpu.SelectedIndex=0;status.Text="找到 "+devices.Count+" 个 Vulkan 设备；请选择设备后初始化";}
                else {gpu.Text="";status.Text="没有可用的 Vulkan 设备，请检查显卡驱动";}
                Text="lw.PPOCR.Vulkan "+Native.Utf8(Native.lwvk_version())+" — WinForms 测试";
                dirty=true;
            } finally {loading=false;UpdateEnabled();}
        }
        private uint DeviceIndex() {
            if(gpu.SelectedItem is DeviceInfo && gpu.Text==gpu.SelectedItem.ToString()) return ((DeviceInfo)gpu.SelectedItem).device_index;
            uint value;if(!uint.TryParse(gpu.Text.Trim(),out value)) throw new ArgumentException("GPU 请选择列表中的设备，或直接输入编号（如 0、1）。");
            if(!devices.Exists(delegate(DeviceInfo info) {return info.device_index==value;}))
                throw new ArgumentException("GPU 编号 "+value+" 不存在。请刷新设备列表核对；不会自动更换设备。");
            return value;
        }
        private void InitializeEngine(Action after) {
            OcrConfig config;Native.Check(Native.lwvk_ocr_config_default(out config));config.device_index=DeviceIndex();
            if((uint)side.Value%32!=0) throw new ArgumentException("DET 长边上限必须是 32 的倍数。");
            config.det_limit_side=(uint)side.Value;config.bitmap_threshold=(float)bitmapThreshold.Value;
            config.max_workspace_bytes=(ulong)workspace.Value*1024*1024;
            config.box_threshold=(float)boxThreshold.Value;config.unclip_ratio=(float)unclip.Value;
            config.enable_classifier=cls.Checked?1u:0;config.cls_threshold=(float)clsThreshold.Value;config.use_dilation=dilation.Checked?1u:0;
            string root=modelRoot.Text;
            if(!Path.IsPathRooted(root)) root=Path.Combine(options.Root,root);
            gpuDisplay=devices.Find(delegate(DeviceInfo d) {return d.device_index==config.device_index;}).ToString();
            var old=session;session=null;dirty=true;status.Text="正在加载模型："+gpuDisplay;
            var clock=Stopwatch.StartNew();
            StartJob(delegate {if(old!=null) old.Dispose();return new OcrSession(root,config);},delegate(OcrSession value) {
                session=value;dirty=false;status.Text=string.Format("初始化完成 · {0} · {1:F1} ms",gpuDisplay,clock.Elapsed.TotalMilliseconds);
            },after);
        }
        private void LoadImage(string path) {
            var image=ImagePixels.Load(path);var old=canvas.Image;canvas.Image=image;if(old!=null) old.Dispose();
            canvas.Boxes.Clear();canvas.SelectRectangle(Rectangle.Empty);canvas.Invalidate();lastFull=null;lastJson=lastText="";
            text.Clear();json.Clear();grid.Rows.Clear();UpdateImageInfo();UpdateEnabled();
            jsonDirty=gridDirty=false;
        }
        private void UpdateImageInfo() {
            var i=canvas.Image;var r=canvas.Selection;
            imageInfo.Text=i==null?"请选择图片":string.Format("{0} × {1}  |  {2}",i.Width,i.Height,
                r.IsEmpty?"鼠标左键框选文字区域":string.Format("ROI: x={0}, y={1}, w={2}, h={3}",r.X,r.Y,r.Width,r.Height));
        }
        private void RunFull(Action after) {
            RequireReady();var clock=Stopwatch.StartNew();var image=(Bitmap)canvas.Image.Clone();var engine=session;
            status.Text="正在整图 OCR："+gpuDisplay;
            StartJob(delegate {using(image) return engine.Run(image);},delegate(FullResult value) {
                lastFull=value;canvas.Boxes=value.Output.items;canvas.Highlight=-1;canvas.Invalidate();
                var lines=new StringBuilder();foreach(var item in value.Output.items) lines.AppendLine(item.text);
                lastText=lines.ToString();lastJson=value.Json;text.Text=lastText;
                jsonDirty=gridDirty=true;RefreshDetails();clock.Stop();lastUiReadyMilliseconds=clock.Elapsed.TotalMilliseconds;
                var t=value.Output.timing;double stages=t.det_ms+t.cls_ms+t.rec_ms;
                status.Text=string.Format("{0} 个区域 · OCR 调用 {1:F1} ms · 界面就绪 {2:F1} ms · {3}\n流水线 {4:F1} ms · DET {5:F1} / CLS {6:F1} / REC {7:F1} · 其他 {8:F1} ms",
                    value.Output.items.Count,value.ClientMilliseconds,lastUiReadyMilliseconds,value.FirstCall?"首轮调用":"重复调用",
                    t.total_ms,t.det_ms,t.cls_ms,t.rec_ms,Math.Max(0,t.total_ms-stages));
            },after);
        }
        private void RunRecognition(Action after) {
            RequireReady();var roi=canvas.Selection;if(roi.Width<2 || roi.Height<2) throw new ArgumentException("请先用鼠标框选文字区域。");
            var crop=canvas.Image.Clone(roi,PixelFormat.Format24bppRgb);var engine=session;var clock=Stopwatch.StartNew();
            status.Text="正在仅识别框选区域（不执行 DET / CLS）";
            StartJob(delegate {using(crop) return engine.Recognize(crop);},delegate(RecResult value) {
                lastText=value.Text;lastJson=new JavaScriptSerializer().Serialize(new {operation="recognize_only",
                    roi=new {x=roi.X,y=roi.Y,width=roi.Width,height=roi.Height},text=value.Text,score=value.Score,
                    gpu_rec_ms=value.Milliseconds,wall_ms=clock.Elapsed.TotalMilliseconds});
                text.Text=lastText;json.Text=Pretty(lastJson);grid.Rows.Clear();grid.Rows.Add("ROI",value.Text,value.Score.ToString("F4"));
                jsonDirty=gridDirty=false;
                status.Text=string.Format("仅识别 ROI · 置信度 {0:F4} · 本次 {1:F1} ms · GPU REC {2:F1} ms（无 DET / CLS）",
                    value.Score,clock.Elapsed.TotalMilliseconds,value.Milliseconds);
            },after);
        }
        private void RequireReady() {
            if(session==null || dirty) throw new InvalidOperationException("请先初始化；参数修改后需要重载引擎。");
            if(canvas.Image==null) throw new InvalidOperationException("请先选择图片。");
        }
        private void FillGrid(List<OcrItem> values) {grid.Rows.Clear();for(int i=0;i<values.Count;++i) grid.Rows.Add(i+1,values[i].text,values[i].score.ToString("F4"));}
        private void RefreshDetails() {
            if(tabs.SelectedIndex==1 && jsonDirty) {json.Text=Pretty(lastJson);jsonDirty=false;}
            if(tabs.SelectedIndex==2 && gridDirty && lastFull!=null) {FillGrid(lastFull.Output.items);gridDirty=false;}
        }
        private void UpdateEnabled() {
            if(IsDisposed) return;
            modelVariant.Enabled=gpu.Enabled=modelRoot.Enabled=browse.Enabled=refresh.Enabled=side.Enabled=bitmapThreshold.Enabled=
                boxThreshold.Enabled=unclip.Enabled=cls.Enabled=clsThreshold.Enabled=dilation.Enabled=workspace.Enabled=open.Enabled=!busy;
            init.Enabled=!busy && devices.Count>0;release.Enabled=!busy && session!=null;
            run.Enabled=!busy && !dirty && session!=null && canvas.Image!=null;
            recognize.Enabled=run.Enabled && !canvas.Selection.IsEmpty;clearRoi.Enabled=!busy && !canvas.Selection.IsEmpty;
            canvas.Enabled=!busy;copy.Enabled=!busy && lastText.Length>0;save.Enabled=!busy && lastJson.Length>0;
            UseWaitCursor=busy;
        }
        private void StartJob<T>(Func<T> worker,Action<T> success,Action after) {
            if(busy) return;busy=true;UpdateEnabled();
            Task.Factory.StartNew(worker).ContinueWith(delegate(Task<T> task) {
                bool ok=false;
                try {if(task.IsFaulted) throw task.Exception.GetBaseException();success(task.Result);ok=true;}
                catch(Exception ex) {Failure(ex);}
                finally {busy=false;UpdateEnabled();}
                if(closePending) Close();else if(ok && after!=null) TryUi(after);
            },CancellationToken.None,TaskContinuationOptions.None,TaskScheduler.FromCurrentSynchronizationContext());
        }
        private void TryUi(Action action) {try {action();} catch(Exception ex) {Failure(ex);}}
        private void Failure(Exception ex) {
            status.Text="失败："+ex.Message;
            if(options.Smoke) {ExitCode=1;WriteReport(new {ok=false,error=ex.ToString()});closePending=true;if(!busy) Close();}
            else if(!closePending) MessageBox.Show(this,ex.Message+"\n\n请核对 GPU 编号、模型目录，以及程序旁的 lw.PPOCR.Vulkan.dll / 显卡驱动。",
                "测试失败",MessageBoxButtons.OK,MessageBoxIcon.Error);
        }
        private void OnClosingForm(object sender,FormClosingEventArgs e) {
            if(busy) {e.Cancel=true;closePending=true;status.Text="正在等待当前原生调用结束后关闭；不会强行释放 GPU 资源。";return;}
            if(session!=null) {session.Dispose();session=null;}
            if(canvas.Image!=null) {canvas.Image.Dispose();canvas.Image=null;}
        }
        private static string Pretty(string value) {
            var output=new StringBuilder();bool quoted=false,escaped=false;int indent=0;
            foreach(char c in value) {
                if(quoted) {output.Append(c);if(escaped) escaped=false;else if(c=='\\') escaped=true;else if(c=='"') quoted=false;continue;}
                if(c=='"') {quoted=true;output.Append(c);}
                else if(c=='{' || c=='[') {output.Append(c).AppendLine();++indent;output.Append(' ',indent*2);}
                else if(c=='}' || c==']') {output.AppendLine();--indent;output.Append(' ',indent*2).Append(c);}
                else if(c==',') {output.Append(c).AppendLine().Append(' ',indent*2);}
                else if(c==':') output.Append(": ");else if(!char.IsWhiteSpace(c)) output.Append(c);
            }
            return output.ToString();
        }
        private void Smoke() {
            if(canvas.Image==null) throw new InvalidOperationException("Smoke: packaged sample image missing");
            // Exercise both tight BGR rows and rows with alignment padding.
            foreach(int width in new[] {4,5}) using(var test=new Bitmap(width,3,PixelFormat.Format24bppRgb)) {
                for(int y=0;y<3;++y) for(int x=0;x<width;++x) test.SetPixel(x,y,Color.FromArgb(10+x,30+y,50+x+y));
                var pixels=ImagePixels.Bgr(test);
                for(int y=0;y<3;++y) for(int x=0;x<width;++x) {
                    int i=(y*width+x)*3;
                    if(pixels[i]!=50+x+y || pixels[i+1]!=30+y || pixels[i+2]!=10+x)
                        throw new InvalidOperationException("BGR contiguous/padded copy mismatch");
                }
            }
            // Editable GPU input contract is tested independently of hardware availability.
            if(devices.Count>0) {
                gpu.SelectedIndex=0;uint selected=DeviceIndex();gpu.Text=selected.ToString();
                if(DeviceIndex()!=selected) throw new InvalidOperationException("Typed GPU ID differs from selected ID");
                gpu.Text="4294967295";bool rejected=false;try {DeviceIndex();} catch(ArgumentException) {rejected=true;}
                if(!rejected) throw new InvalidOperationException("Invalid GPU ID wasn't rejected");
                gpu.Text=selected.ToString();
            }
            var roi=new Rectangle(20,28,292,46);canvas.TestDrag(roi,false);CheckRoi(roi);
            canvas.TestDrag(roi,true);CheckRoi(roi);
            ClientSize=new Size(1060,740);PerformLayout();canvas.TestDrag(roi,true);CheckRoi(roi);
            ClientSize=new Size(1180,800);PerformLayout();canvas.TestDrag(roi,false);CheckRoi(roi);
            Point p;if(canvas.ImagePoint(new Point(-10,-10),out p,false)) throw new InvalidOperationException("Letterbox/outside accepted");
            if(options.HostOnly) {SaveScreenshot();WriteReport(new {ok=true,version=Native.Utf8(Native.lwvk_version()),mode="host-only",devices=devices.Count,
                roi=canvas.Selection.ToString(),gpu_editable="passed",mouse_mapping="normal/reverse/resize passed",gpu_inference="not run"});Close();return;}
            gpu.Text=options.Device.ToString();InitializeEngine(delegate {SmokeFull(new List<object>(),null);});
        }
        private static bool SameItem(OcrItem a,OcrItem b) {
            return a.text==b.text && a.cls_label==b.cls_label && a.score==b.score && a.cls_score==b.cls_score && a.det_score==b.det_score &&
                a.x1==b.x1 && a.y1==b.y1 && a.x2==b.x2 && a.y2==b.y2 && a.x3==b.x3 && a.y3==b.y3 && a.x4==b.x4 && a.y4==b.y4;
        }
        private void SmokeFull(List<object> measurements,List<OcrItem> expectedItems) {
            RunFull(delegate {
                if(lastFull.Output.items.Count!=16 || lastFull.Output.items[0].text!="纯臻营养护发素" || lastFull.Output.items[0].x2>315)
                    throw new InvalidOperationException("Full OCR sample/coordinates mismatch");
                var full=lastFull;
                // JSON object property order is not a correctness contract.
                if(expectedItems!=null) for(int i=0;i<expectedItems.Count;++i)
                    if(!SameItem(expectedItems[i],full.Output.items[i])) throw new InvalidOperationException("Repeated OCR item changed: "+i);
                if(full.ClientMilliseconds<full.NativeCallMilliseconds || full.NativeCallMilliseconds<full.Output.timing.total_ms)
                    throw new InvalidOperationException("Invalid timing boundaries");
                measurements.Add(new {first_call=full.FirstCall,client_ms=full.ClientMilliseconds,
                    native_call_ms=full.NativeCallMilliseconds,ui_ready_ms=lastUiReadyMilliseconds,timing=full.Output.timing});
                if(measurements.Count<options.SmokeRepeat) {SmokeFull(measurements,expectedItems??full.Output.items);return;}
                tabs.SelectedIndex=1;
                if(new JavaScriptSerializer().Deserialize<OcrOutput>(json.Text).items.Count!=16) throw new InvalidOperationException("Lazy JSON mismatch");
                tabs.SelectedIndex=2;if(grid.Rows.Count!=16) throw new InvalidOperationException("Lazy grid mismatch");
                tabs.SelectedIndex=0;SaveScreenshot();RunRecognition(delegate {
                    if(lastText!="纯臻营养护发素") throw new InvalidOperationException("ROI text mismatch: "+lastText);
                    WriteReport(new {ok=true,version=Native.Utf8(Native.lwvk_version()),mode="physical-vulkan",device=options.Device,gpu=gpuDisplay,
                        full_items=full.Output.items.Count,title=full.Output.items[0].text,roi_text=lastText,
                        editable_gpu="passed (selection / typed ID / invalid ID)",mouse_mapping="normal/reverse/resize passed",
                        image_pixels="original not painted",full_timing=full.Output.timing,
                        lazy_details="JSON and grid passed",measurements=measurements});Close();
                });
            });
        }
        private void CheckRoi(Rectangle expected) {
            var r=canvas.Selection;
            if(Math.Abs(r.X-expected.X)>1 || Math.Abs(r.Y-expected.Y)>1 || Math.Abs(r.Width-expected.Width)>2 || Math.Abs(r.Height-expected.Height)>2)
                throw new InvalidOperationException("ROI zoom mapping mismatch: "+r);
        }
        private void SaveScreenshot() {
            if(options.Screenshot==null) return;Directory.CreateDirectory(Path.GetDirectoryName(options.Screenshot));
            using(var image=new Bitmap(Width,Height)) {DrawToBitmap(image,new Rectangle(0,0,Width,Height));image.Save(options.Screenshot,ImageFormat.Png);}
        }
        private void WriteReport(object report) {
            if(options.Report==null) return;Directory.CreateDirectory(Path.GetDirectoryName(options.Report));
            File.WriteAllText(options.Report,new JavaScriptSerializer().Serialize(report),new UTF8Encoding(false));
        }
    }
}
