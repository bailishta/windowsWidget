#pragma once
#include <deque>
#include <winrt/Microsoft.UI.Xaml.Shapes.h>
namespace companion {
class PerformancePlot {
    std::deque<double> history;
    winrt::Windows::UI::Color color;
    winrt::event_token size_changed{};
    void draw() {
        using namespace winrt::Microsoft::UI::Xaml;
        double width=canvas.ActualWidth(),height=canvas.Height();if(width<1)return;
        canvas.Children().Clear();
        auto grid=Media::SolidColorBrush(winrt::Windows::UI::Color{60,color.R,color.G,color.B});
        auto line=[&](double x1,double y1,double x2,double y2){Shapes::Line l;l.X1(x1);l.Y1(y1);l.X2(x2);l.Y2(y2);l.Stroke(grid);l.StrokeThickness(.5);canvas.Children().Append(l);};
        for(double x=0;x<=width;x+=10)line(x,0,x,height);
        for(double y=0;y<=height;y+=10)line(0,y,width,y);line(width,0,width,height);line(0,height,width,height);
        if(history.empty())return;
        Shapes::Polyline curve;curve.Stroke(Media::SolidColorBrush(color));curve.StrokeThickness(1.3);
        Shapes::Polygon fill;fill.Fill(Media::SolidColorBrush(winrt::Windows::UI::Color{28,color.R,color.G,color.B}));
        double first=width-double(history.size()-1)*width/59.0;fill.Points().Append({float(first),float(height)});
        for(size_t i=0;i<history.size();++i){winrt::Windows::Foundation::Point p{float(first+i*width/59.0),float(height*(1-history[i]/100.0))};curve.Points().Append(p);fill.Points().Append(p);}
        fill.Points().Append({float(width),float(height)});canvas.Children().Append(fill);canvas.Children().Append(curve);
    }
public:
    winrt::Microsoft::UI::Xaml::Controls::Canvas canvas;
    explicit PerformancePlot(winrt::Windows::UI::Color value):color(value) {
        canvas.Height(64);canvas.IsHitTestVisible(false);size_changed=canvas.SizeChanged([this](auto const&,auto const&){draw();});
        canvas.Background(winrt::Microsoft::UI::Xaml::Media::SolidColorBrush(winrt::Windows::UI::Color{10,color.R,color.G,color.B}));
    }
    ~PerformancePlot(){canvas.SizeChanged(size_changed);}
    void sample(double value){if(!std::isfinite(value))return;history.push_back(std::clamp(value,0.0,100.0));if(history.size()>60)history.pop_front();draw();}
    size_t size() const{return history.size();}
    double latest() const{return history.empty()?0:history.back();}
};
}
