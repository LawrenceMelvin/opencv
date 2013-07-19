#include "precomp.hpp"
#include <climits>
#include <algorithm>
#include <cstdarg>

#define ALEX_DEBUG

namespace cv{namespace optim{
using std::vector;
using namespace std;

#ifdef ALEX_DEBUG
#define dprintf(x) printf x
static void print_matrix(const Mat& x){
    print(x);
    printf("\n");
}
static void print_simplex_state(const Mat& c,const Mat& b,double v,const std::vector<int> N,const std::vector<int> B){
    printf("\tprint simplex state\n");
    
    printf("v=%g\n",v);
    
    printf("here c goes\n");
    print_matrix(c);
    
    printf("non-basic: ");
    print(Mat(N));
    printf("\n");
    
    printf("here b goes\n");
    print_matrix(b);
    printf("basic: ");
    
    print(Mat(B));
    printf("\n");
}
#else
#define dprintf(x)
#define print_matrix(x)
#define print_simplex_state(c,b,v,N,B)
#endif

/**Due to technical considerations, the format of input b and c is somewhat special:
 *both b and c should be one column bigger than corresponding b and c of linear problem and the leftmost column will be used internally
 by this procedure - it should not be cleaned before the call to procedure and may contain mess after
 it also initializes N and B and does not make any assumptions about their init values
 * @return SOLVELP_UNFEASIBLE if problem is unfeasible, 0 if feasible.
*/
static int initialize_simplex(Mat_<double>& c, Mat_<double>& b,double& v,vector<int>& N,vector<int>& B);
static inline void pivot(Mat_<double>& c,Mat_<double>& b,double& v,vector<int>& N,vector<int>& B, int leaving_index,int entering_index);
/**@return SOLVELP_UNBOUNDED means the problem is unbdd, SOLVELP_MULTI means multiple solutions, SOLVELP_SINGLE means one solution.
 */
static int inner_simplex(Mat_<double>& c, Mat_<double>& b,double& v,vector<int>& N,vector<int>& B);
static void swap_columns(Mat_<double>& A,int col1,int col2);

//return codes:-2 (no_sol - unbdd),-1(no_sol - unfsbl), 0(single_sol), 1(multiple_sol=>least_l2_norm)
int solveLP(const Mat& Func, const Mat& Constr, Mat& z){
    dprintf(("call to solveLP\n"));

    //sanity check (size, type, no. of channels)
    CV_Assert(Func.type()==CV_64FC1 || Func.type()==CV_32FC1);
    CV_Assert(Constr.type()==CV_64FC1 || Constr.type()==CV_32FC1);
    CV_Assert((Func.rows==1 && (Constr.cols-Func.cols==1))||
            (Func.cols==1 && (Constr.cols-Func.rows==1)));

    //copy arguments for we will shall modify them
    Mat_<double> bigC=Mat_<double>(1,(Func.rows==1?Func.cols:Func.rows)+1),
        bigB=Mat_<double>(Constr.rows,Constr.cols+1);
    if(Func.rows==1){
        Func.convertTo(bigC.colRange(1,bigC.cols),CV_64FC1);
    }else{
        Mat FuncT=Func.t();
        FuncT.convertTo(bigC.colRange(1,bigC.cols),CV_64FC1);
    }
    Constr.convertTo(bigB.colRange(1,bigB.cols),CV_64FC1);
    double v=0;
    vector<int> N,B;

    if(initialize_simplex(bigC,bigB,v,N,B)==SOLVELP_UNFEASIBLE){
        return SOLVELP_UNFEASIBLE;
    }
    Mat_<double> c=bigC.colRange(1,bigC.cols),
        b=bigB.colRange(1,bigB.cols);

    int res=0;
    if((res=inner_simplex(c,b,v,N,B))==SOLVELP_UNBOUNDED){
        return SOLVELP_UNBOUNDED;
    }

    //return the optimal solution
    z.create(c.cols,1,CV_64FC1);
    MatIterator_<double> it=z.begin<double>();
    for(int i=1;i<=c.cols;i++,it++){
        std::vector<int>::iterator pos=B.begin();
        if((pos=std::find(B.begin(),B.end(),i))==B.end()){
            *it=0;
        }else{
            *it=b.at<double>(pos-B.begin(),b.cols-1);
        }
    }

    return res;
}

static int initialize_simplex(Mat_<double>& c, Mat_<double>& b,double& v,vector<int>& N,vector<int>& B){
    N.resize(c.cols);
    N[0]=0;
    for (std::vector<int>::iterator it = N.begin()+1 ; it != N.end(); ++it){
        *it=it[-1]+1;
    }
    B.resize(b.rows);
    B[0]=N.size();
    for (std::vector<int>::iterator it = B.begin()+1 ; it != B.end(); ++it){
        *it=it[-1]+1;
    }
    v=0;

    int k=0;
    {
        double min=DBL_MAX;
        for(int i=0;i<b.rows;i++){
            if(b(i,b.cols-1)<min){
                min=b(i,b.cols-1);
                k=i;
            }
        }
    }

    if(b(k,b.cols-1)>=0){
        N.erase(N.begin());
        return 0;
    }

    Mat_<double> old_c=c.clone();
    c=0;
    c(0,0)=-1;
    for(int i=0;i<b.rows;i++){
        b(i,0)=-1;
    }

    print_simplex_state(c,b,v,N,B);

    dprintf(("\tWE MAKE PIVOT\n"));
    pivot(c,b,v,N,B,k,0);

    print_simplex_state(c,b,v,N,B);

    inner_simplex(c,b,v,N,B);

    dprintf(("\tAFTER INNER_SIMPLEX\n"));
    print_simplex_state(c,b,v,N,B);

    vector<int>::iterator iterator=std::find(B.begin(),B.end(),0);
    if(iterator!=B.end()){
        int iterator_offset=iterator-B.begin();
        if(b(iterator_offset,b.cols-1)>0){
            return SOLVELP_UNFEASIBLE;
        }
        pivot(c,b,v,N,B,iterator_offset,0);
    }

    {
        iterator=std::find(N.begin(),N.end(),0);
        int iterator_offset=iterator-N.begin();
        std::iter_swap(iterator,N.begin());
        swap_columns(c,iterator_offset,0);
        swap_columns(b,iterator_offset,0);
    }

    dprintf(("after swaps\n"));
    print_simplex_state(c,b,v,N,B);

    //start from 1, because we ignore x_0
    c=0;
    v=0;
    for(int I=1;I<old_c.cols;I++){
        if((iterator=std::find(N.begin(),N.end(),I))!=N.end()){
            dprintf(("I=%d from nonbasic\n",I));
            int iterator_offset=iterator-N.begin();
            c(0,iterator_offset)+=old_c(0,I);     
            print_matrix(c);
        }else{
            dprintf(("I=%d from basic\n",I));
            int iterator_offset=std::find(B.begin(),B.end(),I)-B.begin();
            c-=old_c(0,I)*b.row(iterator_offset).colRange(0,b.cols-1);
            v+=old_c(0,I)*b(iterator_offset,b.cols-1);
            print_matrix(c);
        }
    }

    dprintf(("after restore\n"));
    print_simplex_state(c,b,v,N,B);

    N.erase(N.begin());
    return 0;
}

static int inner_simplex(Mat_<double>& c, Mat_<double>& b,double& v,vector<int>& N,vector<int>& B){
    int count=0;
    while(1){
        dprintf(("iteration #%d\n",count));
        count++;

        static MatIterator_<double> pos_ptr;
        int e=-1,pos_ctr=0,min_var=INT_MAX;
        bool all_nonzero=true;
        for(pos_ptr=c.begin();pos_ptr!=c.end();pos_ptr++,pos_ctr++){
            if(*pos_ptr==0){
                all_nonzero=false;
            }
            if(*pos_ptr>0){
                if(N[pos_ctr]<min_var){
                    e=pos_ctr;
                    min_var=N[pos_ctr];
                }
            }
        }
        if(e==-1){
            dprintf(("hello from e==-1\n"));
            print_matrix(c);
            if(all_nonzero==true){
                return SOLVELP_SINGLE;
            }else{
                return SOLVELP_MULTI;
            }
        }

        int l=-1;
        min_var=INT_MAX;
        double min=DBL_MAX;
        int row_it=0;
        MatIterator_<double> min_row_ptr=b.begin();
        for(MatIterator_<double> it=b.begin();it!=b.end();it+=b.cols,row_it++){
            double myite=0;
            //check constraints, select the tightest one, reinforcing Bland's rule
            if((myite=it[e])>0){
                double val=it[b.cols-1]/myite;
                if(val<min || (val==min && B[row_it]<min_var)){
                    min_var=B[row_it];
                    min_row_ptr=it;
                    min=val;
                    l=row_it;
                }
            }
        }
        if(l==-1){
            return SOLVELP_UNBOUNDED;
        }
        dprintf(("the tightest constraint is in row %d with %g\n",l,min));

        pivot(c,b,v,N,B,l,e);

        dprintf(("objective, v=%g\n",v));
        print_matrix(c);
        dprintf(("constraints\n"));
        print_matrix(b);
        dprintf(("non-basic: "));
        print_matrix(Mat(N));
        dprintf(("basic: "));
        print_matrix(Mat(B));
    }
}

static inline void pivot(Mat_<double>& c,Mat_<double>& b,double& v,vector<int>& N,vector<int>& B, int leaving_index,int entering_index){
    double Coef=b(leaving_index,entering_index);
    for(int i=0;i<b.cols;i++){
        if(i==entering_index){
            b(leaving_index,i)=1/Coef;
        }else{
            b(leaving_index,i)/=Coef;
        }
    }

    for(int i=0;i<b.rows;i++){
        if(i!=leaving_index){
            double coef=b(i,entering_index);
            for(int j=0;j<b.cols;j++){
                if(j==entering_index){
                    b(i,j)=-coef*b(leaving_index,j);
                }else{
                    b(i,j)-=(coef*b(leaving_index,j));
                }
            }
        }
    }

    //objective function
    Coef=c(0,entering_index);
    for(int i=0;i<(b.cols-1);i++){
        if(i==entering_index){
            c(0,i)=-Coef*b(leaving_index,i);
        }else{
            c(0,i)-=Coef*b(leaving_index,i);
        }
    }
    dprintf(("v was %g\n",v));
    v+=Coef*b(leaving_index,b.cols-1);
    
    int tmp=N[entering_index];
    N[entering_index]=B[leaving_index];
    B[leaving_index]=tmp;
}

static inline void swap_columns(Mat_<double>& A,int col1,int col2){
    for(int i=0;i<A.rows;i++){
        double tmp=A(i,col1);
        A(i,col1)=A(i,col2);
        A(i,col2)=tmp;
    }
}
}}
