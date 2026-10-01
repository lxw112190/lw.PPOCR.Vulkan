using System;
using System.Collections.Generic;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Windows.Forms;

namespace Lw.PPOCR.VulkanDemo {
    internal sealed class ImageCanvas:Control {
        internal Bitmap Image; // Owned/disposed by MainForm, never modified by overlays.
        internal List<OcrItem> Boxes=new List<OcrItem>();
        internal bool ShowBoxes=true;
        internal int Highlight=-1;
        internal Rectangle Selection {get;private set;}
        internal event EventHandler SelectionChanged;
        private Point start; private bool dragging;
        internal ImageCanvas() {
            DoubleBuffered=true; BackColor=Color.FromArgb(240,243,248); Cursor=Cursors.Cross;
            SetStyle(ControlStyles.ResizeRedraw,true);
        }
        internal RectangleF ImageBounds {
            get {
                if(Image==null || Width<=0 || Height<=0) return RectangleF.Empty;
                float scale=Math.Min((float)Width/Image.Width,(float)Height/Image.Height);
                return new RectangleF((Width-Image.Width*scale)/2,(Height-Image.Height*scale)/2,Image.Width*scale,Image.Height*scale);
            }
        }
        internal bool ImagePoint(Point screen,out Point point,bool clamp) {
            var bounds=ImageBounds; point=Point.Empty;
            if(Image==null || bounds.Width<=0 || bounds.Height<=0 || (!clamp && !bounds.Contains(screen))) return false;
            point=new Point(Math.Max(0,Math.Min(Image.Width,(int)Math.Round((screen.X-bounds.X)*Image.Width/bounds.Width))),
                Math.Max(0,Math.Min(Image.Height,(int)Math.Round((screen.Y-bounds.Y)*Image.Height/bounds.Height))));
            return true;
        }
        internal Point ScreenPoint(Point p) {
            var r=ImageBounds;
            return new Point((int)Math.Round(r.X+p.X*r.Width/Image.Width),(int)Math.Round(r.Y+p.Y*r.Height/Image.Height));
        }
        internal void SelectRectangle(Rectangle value) {
            Selection=Image==null?Rectangle.Empty:Rectangle.Intersect(new Rectangle(0,0,Image.Width,Image.Height),value);
            Invalidate(); if(SelectionChanged!=null) SelectionChanged(this,EventArgs.Empty);
        }
        protected override void OnMouseDown(MouseEventArgs e) {
            base.OnMouseDown(e);
            if(!Enabled || e.Button!=MouseButtons.Left || !ImagePoint(e.Location,out start,false)) return;
            Capture=true; dragging=true; SelectRectangle(Rectangle.Empty);
        }
        protected override void OnMouseMove(MouseEventArgs e) {
            base.OnMouseMove(e); if(!dragging) return;
            Point end; if(!ImagePoint(e.Location,out end,true)) return;
            SelectRectangle(Rectangle.FromLTRB(Math.Min(start.X,end.X),Math.Min(start.Y,end.Y),Math.Max(start.X,end.X),Math.Max(start.Y,end.Y)));
        }
        protected override void OnMouseUp(MouseEventArgs e) {
            base.OnMouseUp(e); if(e.Button!=MouseButtons.Left || !dragging) return;
            OnMouseMove(e); dragging=false; Capture=false;
            if(Selection.Width<2 || Selection.Height<2) SelectRectangle(Rectangle.Empty);
        }
        protected override void OnMouseCaptureChanged(EventArgs e) {base.OnMouseCaptureChanged(e);if(!Capture) dragging=false;}
        protected override void OnPaint(PaintEventArgs e) {
            base.OnPaint(e); if(Image==null) {e.Graphics.DrawString("请选择图片",Font,Brushes.Gray,20,20);return;}
            var r=ImageBounds; e.Graphics.InterpolationMode=InterpolationMode.HighQualityBicubic;
            e.Graphics.DrawImage(Image,r,new RectangleF(0,0,Image.Width,Image.Height),GraphicsUnit.Pixel);
            e.Graphics.SmoothingMode=SmoothingMode.AntiAlias;
            if(ShowBoxes) for(int i=0;i<Boxes.Count;++i) {
                var b=Boxes[i]; float sx=r.Width/Image.Width,sy=r.Height/Image.Height;
                var points=new[] {new PointF(r.X+(float)b.x1*sx,r.Y+(float)b.y1*sy),new PointF(r.X+(float)b.x2*sx,r.Y+(float)b.y2*sy),
                    new PointF(r.X+(float)b.x3*sx,r.Y+(float)b.y3*sy),new PointF(r.X+(float)b.x4*sx,r.Y+(float)b.y4*sy)};
                using(var pen=new Pen(i==Highlight?Color.RoyalBlue:Color.Red,i==Highlight?3:1.5f)) e.Graphics.DrawPolygon(pen,points);
                var location=new PointF(points[0].X,Math.Max(r.Y,points[0].Y-17));
                using(var brush=new SolidBrush(Color.FromArgb(210,255,255,255))) e.Graphics.FillRectangle(brush,location.X,location.Y,24,17);
                e.Graphics.DrawString((i+1).ToString(),Font,Brushes.Firebrick,location);
            }
            if(!Selection.IsEmpty) {
                var box=new RectangleF(r.X+Selection.X*r.Width/Image.Width,r.Y+Selection.Y*r.Height/Image.Height,
                    Selection.Width*r.Width/Image.Width,Selection.Height*r.Height/Image.Height);
                using(var fill=new SolidBrush(Color.FromArgb(30,255,165,0))) e.Graphics.FillRectangle(fill,box);
                using(var pen=new Pen(Color.DarkOrange,2)) {pen.DashStyle=DashStyle.Dash;e.Graphics.DrawRectangle(pen,box.X,box.Y,box.Width,box.Height);}
            }
        }
        // Application-owned smoke test drives these same mouse handlers; no OS input injection.
        internal void TestDrag(Rectangle rectangle,bool reverse) {
            Point a=ScreenPoint(rectangle.Location),b=ScreenPoint(new Point(rectangle.Right,rectangle.Bottom));
            if(reverse) {var t=a;a=b;b=t;}
            OnMouseDown(new MouseEventArgs(MouseButtons.Left,1,a.X,a.Y,0));
            OnMouseMove(new MouseEventArgs(MouseButtons.Left,0,b.X,b.Y,0));
            OnMouseUp(new MouseEventArgs(MouseButtons.Left,1,b.X,b.Y,0));
        }
    }
}
